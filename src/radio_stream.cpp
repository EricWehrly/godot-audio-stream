#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

#include "radio_stream.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <chrono>

using namespace godot;

namespace {
// Icecast opens with a burst of several seconds so a player can start fast.
// That overfills the FIFO, and since production then matches consumption
// exactly, it would sit pinned at the cap forever. Trim back to TARGET when it
// exceeds MAX, discarding the OLDEST audio: one discontinuity during startup
// instead of a fresh gap every time the cap is touched.
constexpr size_t MAX_FIFO_FRAMES = 48000 * 8;
constexpr size_t TARGET_FIFO_FRAMES = 48000 * 3;
// Compact the FIFO once this many frames have been read out.
constexpr size_t COMPACT_THRESHOLD = 65536;
constexpr int SOCKET_READ_CHUNK = 16384;
// minimp3 validates a frame by checking that a sync header follows it, so it
// needs bytes PAST the frame it is decoding. Decoding the moment data arrives
// leaves one frame with nothing after it, validation fails, and minimp3 skips
// it looking for sync -- measured at 43% of the stream discarded and audio
// output stuck at ~69% of real time. Always leave this much tail unread.
constexpr size_t DECODE_LOOKAHEAD = 4096;

// Real, reproduced finding (docs/features/tls-streams.md "Open issue" ->
// resolved): the FIRST-EVER TLS connection in a process can fail with
// "SSL module failed to initialize" (CryptoMbedTLS::get_default_certificates()
// returning null) inside a large, busy project (confirmed absent in this
// repo's own minimal demo). Root cause is still Godot's, not fully
// understood, but empirically narrowed by two direct experiments: a
// same-process main-thread HTTPRequest to any https URL, completed first,
// reliably fixes it -- but running this exact warm-up on a background
// thread does NOT (it hits the identical failure). So it specifically needs
// to run on whichever thread calls RadioStream::open() -- in real usage
// that's Godot's main thread, same as HTTPRequest. std::call_once makes
// every RadioStream instance in the process share one real attempt;
// concurrent openers block briefly rather than each hitting the same
// failure independently.
std::once_flag tls_warmup_flag;

void warm_up_tls_once(const String &p_host, int p_port) {
	std::call_once(tls_warmup_flag, [&]() {
		Ref<StreamPeerTCP> tcp;
		tcp.instantiate();
		if (tcp->connect_to_host(p_host, p_port) != OK) {
			return;
		}
		for (int i = 0; i < 300; i++) {
			tcp->poll();
			if (tcp->get_status() == StreamPeerTCP::STATUS_CONNECTED) {
				break;
			}
			if (tcp->get_status() == StreamPeerTCP::STATUS_ERROR) {
				return;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		if (tcp->get_status() != StreamPeerTCP::STATUS_CONNECTED) {
			return;
		}

		Ref<StreamPeerTLS> tls;
		tls.instantiate();
		if (tls->connect_to_stream(tcp, p_host) != OK) {
			return;
		}
		// Best-effort: whether the handshake itself succeeds or fails
		// (a redirect, a cert issue) doesn't matter -- what matters is that
		// CryptoMbedTLS's default-certificate path got exercised once.
		for (int i = 0; i < 300; i++) {
			tcp->poll();
			tls->poll();
			StreamPeerTLS::Status st = tls->get_status();
			if (st != StreamPeerTLS::STATUS_HANDSHAKING) {
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
	});
}
} // namespace

RadioStream::RadioStream() {
	mp3dec_init(&decoder);
}

RadioStream::~RadioStream() {
	close();
}

void RadioStream::_bind_methods() {
	ClassDB::bind_method(D_METHOD("open", "url"), &RadioStream::open);
	ClassDB::bind_method(D_METHOD("close"), &RadioStream::close);
	ClassDB::bind_method(D_METHOD("pop_frames", "max_frames"), &RadioStream::pop_frames);
	ClassDB::bind_method(D_METHOD("get_frames_available"), &RadioStream::get_frames_available);
	ClassDB::bind_method(D_METHOD("get_sample_rate"), &RadioStream::get_sample_rate);
	ClassDB::bind_method(D_METHOD("get_channels"), &RadioStream::get_channels);
	ClassDB::bind_method(D_METHOD("get_status"), &RadioStream::get_status);
	ClassDB::bind_method(D_METHOD("get_last_error"), &RadioStream::get_last_error);
	ClassDB::bind_method(D_METHOD("get_bytes_received"), &RadioStream::get_bytes_received);
	ClassDB::bind_method(D_METHOD("get_frames_decoded"), &RadioStream::get_frames_decoded);
	ClassDB::bind_method(D_METHOD("get_buffer_trims"), &RadioStream::get_buffer_trims);
	ClassDB::bind_method(D_METHOD("get_input_backlog"), &RadioStream::get_input_backlog);
	ClassDB::bind_method(D_METHOD("get_skipped_bytes"), &RadioStream::get_skipped_bytes);
	ClassDB::bind_method(D_METHOD("get_mp3_frames"), &RadioStream::get_mp3_frames);

	BIND_ENUM_CONSTANT(STATUS_IDLE);
	BIND_ENUM_CONSTANT(STATUS_CONNECTING);
	BIND_ENUM_CONSTANT(STATUS_PLAYING);
	BIND_ENUM_CONSTANT(STATUS_ERROR);
}

bool RadioStream::parse_url(const String &p_url, Url &r_out) {
	String rest = p_url;
	int default_port = 80;
	if (rest.begins_with("http://")) {
		rest = rest.substr(7);
		r_out.is_tls = false;
	} else if (rest.begins_with("https://")) {
		rest = rest.substr(8);
		r_out.is_tls = true;
		default_port = 443;
	}

	int slash = rest.find("/");
	String authority = slash == -1 ? rest : rest.substr(0, slash);
	r_out.path = slash == -1 ? String("/") : rest.substr(slash);

	int colon = authority.find(":");
	if (colon == -1) {
		r_out.host = authority;
		r_out.port = default_port;
	} else {
		r_out.host = authority.substr(0, colon);
		r_out.port = authority.substr(colon + 1).to_int();
	}
	return !r_out.host.is_empty() && r_out.port > 0;
}

bool RadioStream::open(const String &p_url) {
	close();

	Url url;
	if (!parse_url(p_url, url)) {
		fail("Could not parse URL: " + p_url);
		return false;
	}

	if (url.is_tls) {
		// Deliberately BEFORE spawning the worker thread, i.e. still on the
		// caller's thread -- confirmed by direct experiment that a
		// background-thread attempt does NOT fix this (it hits the identical
		// failure), only a caller-thread one does. open() is called from
		// GDScript, so in real usage this runs on Godot's main thread, same
		// as a normal HTTPRequest. Guarded by std::call_once: this blocking
		// call happens at most ONCE per process, on the very first https
		// stream anyone opens -- every later open() (including a different
		// host) is unaffected. See the class doc comment and
		// docs/features/tls-streams.md for the full story.
		warm_up_tls_once(url.host, url.port);
	}

	mp3dec_init(&decoder);
	sample_rate.store(0);
	channels.store(0);
	bytes_received.store(0);
	frames_decoded.store(0);
	buffer_trims.store(0);
	skipped_bytes.store(0);
	mp3_frames.store(0);
	input_backlog.store(0);
	{
		std::lock_guard<std::mutex> lock(pcm_mutex);
		pcm_fifo.clear();
		pcm_read_cursor = 0;
	}

	status.store(STATUS_CONNECTING);
	running.store(true);
	worker = std::thread(&RadioStream::worker_main, this, url);
	return true;
}

void RadioStream::close() {
	running.store(false);
	if (worker.joinable()) {
		worker.join();
	}
	if (status.load() != STATUS_ERROR) {
		status.store(STATUS_IDLE);
	}
}

void RadioStream::fail(const String &p_message) {
	{
		std::lock_guard<std::mutex> lock(error_mutex);
		last_error = p_message;
	}
	status.store(STATUS_ERROR);
}

bool RadioStream::poll_connection(Ref<StreamPeerTCP> p_tcp, Ref<StreamPeerTLS> p_tls) {
	p_tcp->poll();
	if (p_tcp->get_status() != StreamPeerTCP::STATUS_CONNECTED) {
		return false;
	}
	if (p_tls.is_valid()) {
		p_tls->poll();
		if (p_tls->get_status() != StreamPeerTLS::STATUS_CONNECTED) {
			return false;
		}
	}
	return true;
}

void RadioStream::worker_main(Url p_url) {
	Ref<StreamPeerTCP> tcp_peer;
	tcp_peer.instantiate();

	if (tcp_peer->connect_to_host(p_url.host, p_url.port) != OK) {
		fail("connect_to_host failed for " + p_url.host);
		return;
	}

	while (running.load()) {
		tcp_peer->poll();
		StreamPeerTCP::Status st = tcp_peer->get_status();
		if (st == StreamPeerTCP::STATUS_CONNECTED) {
			break;
		}
		if (st == StreamPeerTCP::STATUS_ERROR || st == StreamPeerTCP::STATUS_NONE) {
			fail("TCP connection failed");
			return;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	if (!running.load()) {
		return;
	}

	Ref<StreamPeerTLS> tls_peer;
	Ref<StreamPeer> data_peer = tcp_peer;

	if (p_url.is_tls) {
		tls_peer.instantiate();
		if (tls_peer->connect_to_stream(tcp_peer, p_url.host) != OK) {
			fail("Could not start TLS handshake");
			return;
		}

		const int64_t handshake_deadline_ms = 10000;
		int64_t waited = 0;
		while (running.load()) {
			tcp_peer->poll();
			tls_peer->poll();
			StreamPeerTLS::Status tst = tls_peer->get_status();
			if (tst == StreamPeerTLS::STATUS_CONNECTED) {
				break;
			}
			if (tst == StreamPeerTLS::STATUS_ERROR) {
				fail("TLS handshake failed");
				return;
			}
			if (tst == StreamPeerTLS::STATUS_ERROR_HOSTNAME_MISMATCH) {
				// Not retryable -- the certificate genuinely doesn't match
				// the host we asked for. reconnect-resilience (once it
				// exists) should treat this class of error as terminal
				// rather than retrying, same call tls-streams.md makes for a
				// bad certificate in general.
				fail("TLS certificate hostname mismatch for " + p_url.host);
				return;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			waited += 10;
			if (waited > handshake_deadline_ms) {
				fail("TLS handshake timed out");
				return;
			}
		}
		if (!running.load()) {
			return;
		}
		data_peer = tls_peer;
	}

	std::vector<uint8_t> input;
	if (!http_handshake(data_peer, tcp_peer, tls_peer, p_url, input)) {
		return;
	}

	status.store(STATUS_PLAYING);
	decode_available(input);

	while (running.load()) {
		if (!poll_connection(tcp_peer, tls_peer)) {
			fail("Stream disconnected");
			return;
		}

		int avail = data_peer->get_available_bytes();
		if (avail <= 0) {
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
			continue;
		}

		Array result = data_peer->get_partial_data(std::min(avail, SOCKET_READ_CHUNK));
		int64_t read_error = result[0];
		if (read_error != (int64_t)OK) {
			fail("Socket read error");
			return;
		}
		PackedByteArray chunk = result[1];
		if (chunk.size() == 0) {
			continue;
		}
		bytes_received.fetch_add(chunk.size());
		input.insert(input.end(), chunk.ptr(), chunk.ptr() + chunk.size());
		decode_available(input);
	}
}

bool RadioStream::http_handshake(Ref<StreamPeer> p_data_peer, Ref<StreamPeerTCP> p_tcp,
		Ref<StreamPeerTLS> p_tls, const Url &p_url, std::vector<uint8_t> &r_leftover) {
	// Deliberately no "Icy-MetaData: 1" -- requesting metadata makes Icecast
	// interleave title blocks into the audio every icy-metaint bytes, which
	// corrupts MP3 framing unless de-interleaved. Titles are a follow-up.
	String request = "GET " + p_url.path + " HTTP/1.0\r\n" +
			"Host: " + p_url.host + "\r\n" +
			"User-Agent: godot-audio-stream-poc/0.1\r\n" +
			"Accept: */*\r\n" +
			"Connection: close\r\n\r\n";

	PackedByteArray request_bytes = request.to_utf8_buffer();
	if (p_data_peer->put_data(request_bytes) != OK) {
		fail("Failed to send HTTP request");
		return false;
	}

	std::vector<uint8_t> header;
	const int64_t deadline_ms = 10000;
	int64_t waited = 0;

	while (running.load()) {
		if (!poll_connection(p_tcp, p_tls)) {
			fail("Connection dropped during handshake");
			return false;
		}
		int avail = p_data_peer->get_available_bytes();
		if (avail <= 0) {
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			waited += 10;
			if (waited > deadline_ms) {
				fail("Timed out waiting for HTTP response headers");
				return false;
			}
			continue;
		}

		Array result = p_data_peer->get_partial_data(std::min(avail, SOCKET_READ_CHUNK));
		int64_t read_error = result[0];
		if (read_error != (int64_t)OK) {
			fail("Socket read error during handshake");
			return false;
		}
		PackedByteArray chunk = result[1];
		header.insert(header.end(), chunk.ptr(), chunk.ptr() + chunk.size());

		// Look for the CRLFCRLF that ends the response header block.
		for (size_t i = 3; i < header.size(); i++) {
			if (header[i - 3] == '\r' && header[i - 2] == '\n' &&
					header[i - 1] == '\r' && header[i] == '\n') {
				String status_line;
				for (size_t j = 0; j < i && header[j] != '\r'; j++) {
					status_line += String::chr(header[j]);
				}
				if (!status_line.contains("200")) {
					fail("Server rejected the request: " + status_line);
					return false;
				}
				r_leftover.assign(header.begin() + i + 1, header.end());
				bytes_received.fetch_add((int64_t)r_leftover.size());
				return true;
			}
		}

		if (header.size() > 65536) {
			fail("HTTP response header exceeded 64 KiB");
			return false;
		}
	}
	return false;
}

void RadioStream::decode_available(std::vector<uint8_t> &r_input) {
	short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
	size_t offset = 0;

	// Decode everything available WITHOUT holding the lock. Taking pcm_mutex per
	// MP3 frame let a fast consumer starve this thread badly enough that decode
	// output fell to ~69% of real time and the FIFO never refilled.
	decode_batch.clear();

	while (r_input.size() - offset >= DECODE_LOOKAHEAD) {
		mp3dec_frame_info_t info;
		int samples = mp3dec_decode_frame(&decoder, r_input.data() + offset,
				(int)(r_input.size() - offset), pcm, &info);

		if (info.frame_bytes == 0) {
			break; // Need more bytes before the next frame can be read.
		}
		offset += (size_t)info.frame_bytes;

		if (samples <= 0) {
			// frame_bytes > 0 with no samples means minimp3 skipped junk or an
			// ID3 tag. Normal at stream start, not a decode failure.
			skipped_bytes.fetch_add(info.frame_bytes);
			continue;
		}
		mp3_frames.fetch_add(1);

		sample_rate.store(info.hz);
		channels.store(info.channels);

		for (int i = 0; i < samples; i++) {
			float l, r;
			if (info.channels >= 2) {
				l = pcm[i * 2] / 32768.0f;
				r = pcm[i * 2 + 1] / 32768.0f;
			} else {
				l = r = pcm[i] / 32768.0f;
			}
			decode_batch.push_back(Vector2(l, r));
		}
	}

	if (offset > 0) {
		r_input.erase(r_input.begin(), r_input.begin() + offset);
	}
	input_backlog.store((int)r_input.size());

	if (decode_batch.empty()) {
		return;
	}

	std::lock_guard<std::mutex> lock(pcm_mutex);
	pcm_fifo.insert(pcm_fifo.end(), decode_batch.begin(), decode_batch.end());
	frames_decoded.fetch_add((int64_t)decode_batch.size());

	size_t available = pcm_fifo.size() - pcm_read_cursor;
	if (available > MAX_FIFO_FRAMES) {
		pcm_read_cursor += available - TARGET_FIFO_FRAMES;
		buffer_trims.fetch_add(1);
	}
}

PackedVector2Array RadioStream::pop_frames(int p_max_frames) {
	PackedVector2Array out;
	if (p_max_frames <= 0) {
		return out;
	}

	std::lock_guard<std::mutex> lock(pcm_mutex);
	size_t available = pcm_fifo.size() - pcm_read_cursor;
	size_t count = std::min(available, (size_t)p_max_frames);
	if (count == 0) {
		return out;
	}

	out.resize((int64_t)count);
	for (size_t i = 0; i < count; i++) {
		out.set((int64_t)i, pcm_fifo[pcm_read_cursor + i]);
	}
	pcm_read_cursor += count;

	if (pcm_read_cursor > COMPACT_THRESHOLD) {
		pcm_fifo.erase(pcm_fifo.begin(), pcm_fifo.begin() + pcm_read_cursor);
		pcm_read_cursor = 0;
	}
	return out;
}

int RadioStream::get_frames_available() const {
	std::lock_guard<std::mutex> lock(pcm_mutex);
	return (int)(pcm_fifo.size() - pcm_read_cursor);
}

int RadioStream::get_sample_rate() const {
	return sample_rate.load();
}

int RadioStream::get_channels() const {
	return channels.load();
}

int RadioStream::get_status() const {
	return status.load();
}

String RadioStream::get_last_error() const {
	std::lock_guard<std::mutex> lock(error_mutex);
	return last_error;
}

int64_t RadioStream::get_bytes_received() const {
	return bytes_received.load();
}

int64_t RadioStream::get_frames_decoded() const {
	return frames_decoded.load();
}

int RadioStream::get_buffer_trims() const {
	return buffer_trims.load();
}

int RadioStream::get_input_backlog() const {
	return input_backlog.load();
}

int64_t RadioStream::get_skipped_bytes() const {
	return skipped_bytes.load();
}

int64_t RadioStream::get_mp3_frames() const {
	return mp3_frames.load();
}
