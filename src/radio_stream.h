#pragma once

// RadioStream: pulls a continuous Icecast/Shoutcast MP3 stream and hands decoded
// PCM to the caller. Owns exactly three things:
//   1. the HTTP GET + response-header handshake (raw StreamPeerTCP)
//   2. a worker thread: socket read -> incremental minimp3 decode
//   3. a mutex-guarded PCM FIFO that GDScript drains via pop_frames()
// It deliberately knows nothing about Godot's audio graph -- feeding
// AudioStreamGenerator is the caller's job (see demo/main.gd).

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/stream_peer.hpp>
#include <godot_cpp/classes/stream_peer_tcp.hpp>
#include <godot_cpp/classes/stream_peer_tls.hpp>
#include <godot_cpp/core/binder_common.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "minimp3.h"

class RadioStream : public godot::RefCounted {
	GDCLASS(RadioStream, godot::RefCounted)

public:
	enum Status {
		STATUS_IDLE = 0,
		STATUS_CONNECTING,
		STATUS_PLAYING,
		STATUS_ERROR,
	};

	RadioStream();
	~RadioStream();

	// Spawns the worker. Returns false if the URL could not be parsed.
	bool open(const godot::String &p_url);
	void close();

	// Drains up to p_max_frames stereo frames. Short reads are normal.
	godot::PackedVector2Array pop_frames(int p_max_frames);
	int get_frames_available() const;

	// 0 until the first MP3 frame decodes -- the caller must wait for a
	// nonzero rate before configuring AudioStreamGenerator.mix_rate.
	int get_sample_rate() const;
	int get_channels() const;

	int get_status() const;
	godot::String get_last_error() const;
	int64_t get_bytes_received() const;
	int64_t get_frames_decoded() const;
	// Times the FIFO overflowed and old audio was discarded to stay live.
	// Expect a small number during startup burst, then none.
	int get_buffer_trims() const;
	// Undecoded compressed bytes still queued. Distinguishes a slow network
	// (stays ~0) from a slow decoder (grows without bound).
	int get_input_backlog() const;
	// Bytes minimp3 consumed without producing samples (junk/ID3/resync).
	int64_t get_skipped_bytes() const;
	// MP3 frames that actually yielded audio.
	int64_t get_mp3_frames() const;

protected:
	static void _bind_methods();

private:
	struct Url {
		godot::String host;
		godot::String path;
		int port = 80;
		bool is_tls = false;
	};

	static bool parse_url(const godot::String &p_url, Url &r_out);

	void worker_main(Url p_url);
	// Polls whichever layer(s) are active and reports whether the connection
	// is still up. TLS wraps TCP, so both need polling every tick: TCP pumps
	// the raw socket, TLS pumps its own handshake/record layer on top of it.
	// p_tls may be null (plain http), in which case only the TCP status
	// matters.
	static bool poll_connection(godot::Ref<godot::StreamPeerTCP> p_tcp, godot::Ref<godot::StreamPeerTLS> p_tls);
	bool http_handshake(godot::Ref<godot::StreamPeer> p_data_peer, godot::Ref<godot::StreamPeerTCP> p_tcp,
			godot::Ref<godot::StreamPeerTLS> p_tls, const Url &p_url, std::vector<uint8_t> &r_leftover);
	void decode_available(std::vector<uint8_t> &r_input);
	void fail(const godot::String &p_message);

	std::thread worker;
	std::atomic<bool> running{ false };
	std::atomic<int> status{ STATUS_IDLE };

	mutable std::mutex pcm_mutex;
	std::vector<godot::Vector2> pcm_fifo;
	size_t pcm_read_cursor = 0;

	// Decoded outside the lock, then appended to pcm_fifo in one go. Reused
	// across calls so a steady stream stops reallocating.
	std::vector<godot::Vector2> decode_batch;

	mutable std::mutex error_mutex;
	godot::String last_error;

	mp3dec_t decoder;
	std::atomic<int> sample_rate{ 0 };
	std::atomic<int> channels{ 0 };
	std::atomic<int64_t> bytes_received{ 0 };
	std::atomic<int64_t> frames_decoded{ 0 };
	std::atomic<int> buffer_trims{ 0 };
	std::atomic<int> input_backlog{ 0 };
	std::atomic<int64_t> skipped_bytes{ 0 };
	std::atomic<int64_t> mp3_frames{ 0 };
};

VARIANT_ENUM_CAST(RadioStream::Status);
