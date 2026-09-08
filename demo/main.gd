extends Node

## POC harness: drives RadioStream (C++ decode) into AudioStreamGenerator (GDScript audio).
## The number that matters is `underruns` -- it must stay 0 across a long run.

## Any Icecast/Shoutcast endpoint serving raw MP3 over plain http.
## NOTE: using a third party's stream in a shipped game is a ToS/licensing
## question separate from whether this works technically.
@export var stream_url: String = "http://ice1.somafm.com/groovesalad-128-mp3"

## Seconds of decoded audio to bank before starting playback.
@export var prebuffer_seconds: float = 1.0

@onready var player: AudioStreamPlayer = $Player
@onready var stats: Label = $UI/Stats

var radio: RadioStream
var playback: AudioStreamGeneratorPlayback
var started := false
var underruns := 0
var pushed_frames := 0
var elapsed := 0.0


func _ready() -> void:
	radio = RadioStream.new()
	if not radio.open(stream_url):
		push_error("RadioStream.open failed: %s" % radio.get_last_error())


func _process(delta: float) -> void:
	elapsed += delta
	if started:
		_pump()
	else:
		_try_start()
	_update_stats()


func _try_start() -> void:
	var rate := radio.get_sample_rate()
	if rate <= 0:
		return
	if radio.get_frames_available() < int(rate * prebuffer_seconds):
		return

	var generator: AudioStreamGenerator = player.stream
	generator.mix_rate = float(rate)
	player.play()
	playback = player.get_stream_playback() as AudioStreamGeneratorPlayback
	started = true


func _pump() -> void:
	var room := playback.get_frames_available()
	if room <= 0:
		return
	var frames := radio.pop_frames(room)
	if frames.is_empty():
		if radio.get_status() == RadioStream.STATUS_PLAYING:
			underruns += 1
		return
	playback.push_buffer(frames)
	pushed_frames += frames.size()


func _update_stats() -> void:
	var kbps := 0.0
	if elapsed > 0.0:
		kbps = (float(radio.get_bytes_received()) * 8.0 / 1000.0) / elapsed

	var buffered := 0.0
	var rate := radio.get_sample_rate()
	if rate > 0:
		buffered = float(radio.get_frames_available()) / float(rate)

	stats.text = "\n".join([
		"url            %s" % stream_url,
		"status         %s" % _status_name(radio.get_status()),
		"error          %s" % radio.get_last_error(),
		"",
		"format         %d Hz, %d ch" % [rate, radio.get_channels()],
		"received       %.1f KiB  (%.0f kbps)" % [radio.get_bytes_received() / 1024.0, kbps],
		"decoded        %d frames" % radio.get_frames_decoded(),
		"fifo depth     %.2f s" % buffered,
		"",
		"playing        %s" % ("yes" if started else "prebuffering..."),
		"pushed         %.1f s" % (float(pushed_frames) / maxf(float(rate), 1.0)),
		"elapsed        %.1f s" % elapsed,
		"",
		"UNDERRUNS      %d" % underruns,
		"trims          %d" % radio.get_buffer_trims(),
	])


func _status_name(value: int) -> String:
	match value:
		RadioStream.STATUS_IDLE: return "IDLE"
		RadioStream.STATUS_CONNECTING: return "CONNECTING"
		RadioStream.STATUS_PLAYING: return "PLAYING"
		RadioStream.STATUS_ERROR: return "ERROR"
	return "?"


func _exit_tree() -> void:
	if radio != null:
		radio.close()
