#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

#include "radio_stream.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <chrono>
#include <utility>

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

String RadioStream::resolve_redirect_url(const Url &p_base, const String &p_location) {
	String location = p_location.strip_edges();
	if (location.begins_with("http://") || location.begins_with("https://")) {
		return location;
	}

	String scheme = p_base.is_tls ? "https:" : "http:";
	if (location.begins_with("//")) {
		return scheme + location; // protocol-relative: //host/path
	}

	String base_scheme = p_base.is_tls ? "https://" : "http://";
	String authority = p_base.host;
	bool default_port = (p_base.is_tls && p_base.port == 443) || (!p_base.is_tls && p_base.port == 80);
	if (!default_port) {
		authority += ":" + String::num_int64(p_base.port);
	}

	if (location.begins_with("/")) {
		return base_scheme + authority + location; // absolute path, same host
	}

	// Relative to the current path's own directory -- rare for a redirect,
	// but valid HTTP and cheap to support correctly.
	String dir = p_base.path.get_base_dir();
	if (dir.is_empty()) {
		dir = "/";
	} else if (!dir.ends_with("/")) {
		dir += "/";
	}
	return base_scheme + authority + dir + location;
}

bool RadioStream::looks_like_playlist(const String &p_content_type, const String &p_path) {
	String ct = p_content_type.to_lower();
	if (ct.contains("audio/x-scpls") || ct.contains("application/pls+xml") ||
			ct.contains("audio/x-mpegurl") || ct.contains("application/vnd.apple.mpegurl") ||
			ct.contains("application/x-mpegurl")) {
		return true;
	}
	// Servers are unreliable about Content-Type for playlists; extension is
	// the fallback, same posture as the doc's own scope calls for.
	String path_no_query = p_path.contains("?") ? p_path.substr(0, p_path.find("?")) : p_path;
	String ext = path_no_query.get_extension().to_lower();
	return ext == "pls" || ext == "m3u" || ext == "m3u8";
}

bool RadioStream::looks_like_hls(const String &p_body) {
	// A plain m3u playlist and an HLS segment manifest share the .m3u8
	// extension and the #EXTM3U header; these tags are HLS-specific
	// (RFC 8216) and don't appear in an ordinary station-list playlist.
	return p_body.contains("#EXT-X-STREAM-INF") || p_body.contains("#EXT-X-VERSION") ||
			p_body.contains("#EXT-X-TARGETDURATION");
}

std::vector<String> RadioStream::parse_pls(const String &p_body) {
	// PLS is INI-shaped: FileN=url lines, but nothing guarantees File1
	// appears before File2 in the text -- only the number is authoritative,
	// so entries are collected then sorted by index rather than trusting
	// line order.
	std::vector<std::pair<int, String>> entries;
	PackedStringArray lines = p_body.split("\n");
	for (int i = 0; i < lines.size(); i++) {
		String line = lines[i].strip_edges();
		int eq = line.find("=");
		if (eq <= 0) {
			continue;
		}
		String key = line.substr(0, eq);
		if (!key.to_lower().begins_with("file")) {
			continue;
		}
		String num_part = key.substr(4);
		if (!num_part.is_valid_int()) {
			continue;
		}
		String url = line.substr(eq + 1).strip_edges();
		if (url.is_empty()) {
			continue;
		}
		entries.push_back({ (int)num_part.to_int(), url });
	}
	std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) { return a.first < b.first; });

	std::vector<String> out;
	out.reserve(entries.size());
	for (const auto &e : entries) {
		out.push_back(e.second);
	}
	return out;
}

std::vector<String> RadioStream::parse_m3u(const String &p_body) {
	std::vector<String> out;
	PackedStringArray lines = p_body.split("\n");
	for (int i = 0; i < lines.size(); i++) {
		String line = lines[i].strip_edges();
		if (line.is_empty() || line.begins_with("#")) {
			continue; // comments and #EXTINF/#EXTM3U metadata lines
		}
		out.push_back(line);
	}
	return out;
}

int RadioStream::parse_status_code(const String &p_header_text) {
	int space1 = p_header_text.find(" ");
	if (space1 == -1) {
		return -1;
	}
	int space2 = p_header_text.find(" ", space1 + 1);
	if (space2 == -1) {
		return -1;
	}
	String code_str = p_header_text.substr(space1 + 1, space2 - space1 - 1);
	if (!code_str.is_valid_int()) {
		return -1;
	}
	return code_str.to_int();
}

String RadioStream::find_header(const String &p_header_text, const String &p_name) {
	PackedStringArray lines = p_header_text.split("\r\n");
	String prefix_lower = (p_name + String(":")).to_lower();
	for (int i = 0; i < lines.size(); i++) {
		String line_lower = lines[i].to_lower();
		if (line_lower.begins_with(prefix_lower)) {
			return lines[i].substr(prefix_lower.length()).strip_edges();
		}
	}
	return String();
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
	// The parsed Url above was only for fast-fail validation on the caller's
	// thread. worker_main gets the raw string instead: its resolution loop
	// (url-resolution -- redirects, playlists) parses fresh URLs as it goes,
	// and the original string is just its first candidate.
	worker = std::thread(&RadioStream::worker_main, this, p_url);
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

void RadioStream::worker_main(String p_initial_url) {
	// Redirects and playlist entries both collapse to "here are more URLs to
	// try" (HandshakeOutcome::RESOLVE), so one loop and one visited/hop guard
	// covers both -- a playlist entry that itself redirects, or a redirect to
	// a playlist, falls out naturally instead of needing separate bookkeeping.
	constexpr int MAX_RESOLUTION_HOPS = 10;
	std::deque<String> candidates;
	std::set<String> visited;
	candidates.push_back(p_initial_url);

	String last_reason = "No playable URL found";
	int hops = 0;

	while (running.load()) {
		if (candidates.empty()) {
			fail(last_reason);
			return;
		}
		if (hops >= MAX_RESOLUTION_HOPS) {
			fail("Too many redirects/playlist entries (over " + String::num_int64(MAX_RESOLUTION_HOPS) + ")");
			return;
		}

		String next = candidates.front();
		candidates.pop_front();
		if (visited.count(next) != 0) {
			// Already tried (a redirect loop, or two playlists pointing at
			// each other) -- skip rather than fail immediately, in case
			// other untried candidates remain. Still record a specific
			// reason so a subsequently-empty queue reports the real cause
			// instead of a stale message from an unrelated earlier attempt.
			last_reason = "Redirect loop or repeated entry detected at: " + next;
			continue;
		}
		visited.insert(next);
		hops++;

		Url url;
		if (!parse_url(next, url)) {
			last_reason = "Could not parse URL: " + next;
			continue;
		}

		Ref<StreamPeerTCP> tcp_peer;
		Ref<StreamPeerTLS> tls_peer;
		Ref<StreamPeer> data_peer;
		String connect_error;
		if (!connect_peers(url, tcp_peer, tls_peer, data_peer, connect_error)) {
			if (!running.load()) {
				return;
			}
			last_reason = connect_error;
			continue;
		}
		if (!running.load()) {
			return;
		}

		HandshakeResult result = http_handshake(data_peer, tcp_peer, tls_peer, url);
		if (!running.load()) {
			return;
		}

		if (result.outcome == HandshakeOutcome::FAIL) {
			last_reason = result.error.is_empty() ? ("Request failed for " + next) : result.error;
			continue;
		}

		if (result.outcome == HandshakeOutcome::RESOLVE) {
			// Prepend so these are tried immediately, in the order the
			// redirect/playlist gave them, ahead of whatever else was queued.
			for (auto it = result.next_urls.rbegin(); it != result.next_urls.rend(); ++it) {
				candidates.push_front(*it);
			}
			continue;
		}

		// SUCCESS -- resolution is over, this is the audio. Steady-state read
		// loop below only ever exits via return (disconnect/error/close).
		status.store(STATUS_PLAYING);
		std::vector<uint8_t> input = result.leftover;
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

			Array read_result = data_peer->get_partial_data(std::min(avail, SOCKET_READ_CHUNK));
			int64_t read_error = read_result[0];
			if (read_error != (int64_t)OK) {
				fail("Socket read error");
				return;
			}
			PackedByteArray chunk = read_result[1];
			if (chunk.size() == 0) {
				continue;
			}
			bytes_received.fetch_add(chunk.size());
			input.insert(input.end(), chunk.ptr(), chunk.ptr() + chunk.size());
			decode_available(input);
		}
		return;
	}
}

bool RadioStream::connect_peers(const Url &p_url, Ref<StreamPeerTCP> &r_tcp, Ref<StreamPeerTLS> &r_tls,
		Ref<StreamPeer> &r_data, String &r_error) {
	r_tcp.instantiate();
	if (r_tcp->connect_to_host(p_url.host, p_url.port) != OK) {
		r_error = "connect_to_host failed for " + p_url.host;
		return false;
	}

	// A dead port doesn't reliably reach STATUS_ERROR quickly on its own --
	// measured directly (url-resolution testing): a refused localhost
	// connection can sit in a non-terminal state well past any reasonable
	// wait. This deadline is what the original single-URL open() never
	// needed (a human supplies one real URL and waits), but a playlist
	// fallback trying several candidates absolutely does.
	const int64_t connect_deadline_ms = 8000;
	int64_t connect_waited = 0;
	while (running.load()) {
		r_tcp->poll();
		StreamPeerTCP::Status st = r_tcp->get_status();
		if (st == StreamPeerTCP::STATUS_CONNECTED) {
			break;
		}
		if (st == StreamPeerTCP::STATUS_ERROR || st == StreamPeerTCP::STATUS_NONE) {
			r_error = "TCP connection failed for " + p_url.host;
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
		connect_waited += 10;
		if (connect_waited > connect_deadline_ms) {
			r_error = "TCP connect timed out for " + p_url.host;
			return false;
		}
	}
	if (!running.load()) {
		return false;
	}

	r_data = r_tcp;
	if (!p_url.is_tls) {
		return true;
	}

	r_tls.instantiate();
	if (r_tls->connect_to_stream(r_tcp, p_url.host) != OK) {
		r_error = "Could not start TLS handshake for " + p_url.host;
		return false;
	}

	const int64_t handshake_deadline_ms = 10000;
	int64_t waited = 0;
	while (running.load()) {
		r_tcp->poll();
		r_tls->poll();
		StreamPeerTLS::Status tst = r_tls->get_status();
		if (tst == StreamPeerTLS::STATUS_CONNECTED) {
			break;
		}
		if (tst == StreamPeerTLS::STATUS_ERROR) {
			r_error = "TLS handshake failed for " + p_url.host;
			return false;
		}
		if (tst == StreamPeerTLS::STATUS_ERROR_HOSTNAME_MISMATCH) {
			// Not retryable against THIS host -- but in a multi-candidate
			// resolution (a playlist with several mirrors) a different entry
			// may still be fine, so this is only a per-candidate failure, not
			// an immediate abort of the whole resolution.
			r_error = "TLS certificate hostname mismatch for " + p_url.host;
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
		waited += 10;
		if (waited > handshake_deadline_ms) {
			r_error = "TLS handshake timed out for " + p_url.host;
			return false;
		}
	}
	if (!running.load()) {
		return false;
	}
	r_data = r_tls;
	return true;
}

RadioStream::HandshakeResult RadioStream::http_handshake(Ref<StreamPeer> p_data_peer, Ref<StreamPeerTCP> p_tcp,
		Ref<StreamPeerTLS> p_tls, const Url &p_url) {
	HandshakeResult result;
	result.outcome = HandshakeOutcome::FAIL;

	// Deliberately no "Icy-MetaData: 1" -- requesting metadata makes Icecast
	// interleave title blocks into the audio every icy-metaint bytes, which
	// corrupts MP3 framing unless de-interleaved. Titles are a follow-up.
	String request = "GET " + p_url.path + " HTTP/1.0\r\n" +
			"Host: " + p_url.host + "\r\n" +
			"User-Agent: godot-audio-stream-poc/0.1\r\n" +
			"Accept: */*\r\n" +
			"Connection: close\r\n\r\n";
	if (p_data_peer->put_data(request.to_utf8_buffer()) != OK) {
		return result;
	}

	// Phase 1: read until the CRLFCRLF that ends the response headers.
	std::vector<uint8_t> buffer;
	size_t header_end = 0;
	const int64_t deadline_ms = 10000;
	int64_t waited = 0;
	while (running.load() && header_end == 0) {
		if (!poll_connection(p_tcp, p_tls)) {
			return result;
		}
		int avail = p_data_peer->get_available_bytes();
		if (avail <= 0) {
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			waited += 10;
			if (waited > deadline_ms) {
				return result;
			}
			continue;
		}
		Array read = p_data_peer->get_partial_data(std::min(avail, SOCKET_READ_CHUNK));
		if ((int64_t)read[0] != (int64_t)OK) {
			return result;
		}
		PackedByteArray chunk = read[1];
		buffer.insert(buffer.end(), chunk.ptr(), chunk.ptr() + chunk.size());

		for (size_t i = 3; i < buffer.size(); i++) {
			if (buffer[i - 3] == '\r' && buffer[i - 2] == '\n' && buffer[i - 1] == '\r' && buffer[i] == '\n') {
				header_end = i + 1;
				break;
			}
		}
		if (header_end == 0 && buffer.size() > 65536) {
			return result;
		}
	}
	if (!running.load() || header_end == 0) {
		return result;
	}

	// Phase 2: interpret status + the headers we care about.
	String header_text;
	for (size_t j = 0; j < header_end; j++) {
		header_text += String::chr(buffer[j]);
	}
	int status_code = parse_status_code(header_text);
	String location = find_header(header_text, "Location");
	String content_type = find_header(header_text, "Content-Type");

	if (status_code == 301 || status_code == 302 || status_code == 303 ||
			status_code == 307 || status_code == 308) {
		if (location.is_empty()) {
			result.error = String::num_int64(status_code) + " redirect with no Location header";
			return result;
		}
		result.outcome = HandshakeOutcome::RESOLVE;
		result.next_urls.push_back(resolve_redirect_url(p_url, location));
		return result;
	}

	if (status_code != 200) {
		result.error = "Server returned status " + String::num_int64(status_code);
		return result;
	}

	if (!looks_like_playlist(content_type, p_url.path)) {
		result.outcome = HandshakeOutcome::SUCCESS;
		result.leftover.assign(buffer.begin() + header_end, buffer.end());
		bytes_received.fetch_add((int64_t)result.leftover.size());
		return result;
	}

	// Phase 3 (playlist only): keep reading a bounded body. The server
	// closing the connection here is expected (we asked for Connection:
	// close after fully sending a small text file) and is success, not the
	// error it would be for an audio stream disconnecting mid-stream.
	std::vector<uint8_t> body(buffer.begin() + header_end, buffer.end());
	const size_t MAX_PLAYLIST_BYTES = 65536;
	int64_t body_waited = 0;
	while (running.load() && body.size() < MAX_PLAYLIST_BYTES) {
		if (!poll_connection(p_tcp, p_tls)) {
			break;
		}
		int avail = p_data_peer->get_available_bytes();
		if (avail <= 0) {
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			body_waited += 10;
			if (body_waited > deadline_ms) {
				break;
			}
			continue;
		}
		Array read = p_data_peer->get_partial_data(std::min(avail, SOCKET_READ_CHUNK));
		if ((int64_t)read[0] != (int64_t)OK) {
			break;
		}
		PackedByteArray chunk = read[1];
		body.insert(body.end(), chunk.ptr(), chunk.ptr() + chunk.size());
	}
	if (!running.load()) {
		return result;
	}

	String body_text;
	for (uint8_t b : body) {
		body_text += String::chr(b);
	}

	// Regardless of extension/content-type -- if the body itself carries HLS
	// markers, reject it (D5). A mislabeled .m3u3 could otherwise slip past
	// an extension-only check.
	if (looks_like_hls(body_text)) {
		result.error = "HLS (segment-manifest) streams are not supported";
		return result;
	}

	String path_no_query = p_url.path.contains("?") ? p_url.path.substr(0, p_url.path.find("?")) : p_url.path;
	String ext = path_no_query.get_extension().to_lower();
	String ct_lower = content_type.to_lower();
	bool is_pls = ext == "pls" || ct_lower.contains("scpls") || ct_lower.contains("pls+xml");

	std::vector<String> entries = is_pls ? parse_pls(body_text) : parse_m3u(body_text);
	if (entries.empty()) {
		result.error = "Playlist had no usable entries";
		return result;
	}
	result.outcome = HandshakeOutcome::RESOLVE;
	result.next_urls = entries;
	return result;
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
