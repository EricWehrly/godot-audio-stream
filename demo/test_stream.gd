extends SceneTree

## Headless soak test of the C++ half: connect to a live station, decode, and
## drain the FIFO at exactly the stream's own sample rate to simulate a
## real-time consumer. Starvation here is the same failure an AudioStreamGenerator
## underrun would be, minus the audio device -- so this is testable in CI/headless.
##
## Run: <godot> --headless --path demo -s test_stream.gd

const URL := "http://ice1.somafm.com/groovesalad-128-mp3"
const RUN_SECONDS := 45.0
const PREBUFFER_SECONDS := 1.0
const CONNECT_TIMEOUT := 20.0
## Point after which the buffer should have settled and stopped trimming.
const STEADY_AFTER := 15.0

var radio: RadioStream
var elapsed := 0.0
var play_time := 0.0
var started := false
var starvations := 0
var consumed := 0
var next_report := 5.0
var frames := 0
var last_bytes := 0
## Fractional carry: int(delta * rate) alone truncates every tick, which at a
## high frame rate silently understates consumption by a large margin.
var frame_debt := 0.0
var steady_trims := -1
var run_seconds := RUN_SECONDS


func _init() -> void:
	Engine.max_fps = 120
	# Godot passes anything after "++" through as user args.
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--seconds="):
			run_seconds = float(arg.split("=")[1])

	radio = RadioStream.new()
	print("connecting to %s for %.0fs" % [URL, run_seconds])
	if not radio.open(URL):
		print("RESULT  FAIL (open rejected the URL: %s)" % radio.get_last_error())
		quit(1)


func _process(delta: float) -> bool:
	elapsed += delta
	var rate := radio.get_sample_rate()

	if not started:
		return _await_prebuffer(rate)

	play_time += delta
	frames += 1
	frame_debt += delta * rate
	var want := int(frame_debt)
	if want > 0:
		frame_debt -= float(want)
		var got := radio.pop_frames(want).size()
		consumed += got
		if got < want:
			starvations += 1

	if elapsed >= next_report:
		next_report += 5.0
		var now_bytes := radio.get_bytes_received()
		# Per-interval kbps is the number that matters: an average is polluted by
		# Icecast's burst-on-connect, which hides the true steady-state rate.
		var interval_kbps := float(now_bytes - last_bytes) * 8.0 / 1000.0 / 5.0
		last_bytes = now_bytes
		print("  t=%4.1fs  fifo=%5.2fs  audio=%6.2fs  net=%6.1fkbps  backlog=%5dB  skip=%7d  starv=%d" % [
			elapsed, float(radio.get_frames_available()) / float(rate),
			float(radio.get_frames_decoded()) / float(rate), interval_kbps,
			radio.get_input_backlog(), radio.get_skipped_bytes(), starvations,
		])

	# Trims during the opening burst are correct behaviour; trims once the
	# stream has settled would mean a recurring audible gap. Only judge the latter.
	if steady_trims < 0 and elapsed >= STEADY_AFTER:
		steady_trims = radio.get_buffer_trims()

	if elapsed >= run_seconds:
		return _report(rate)
	return false


func _await_prebuffer(rate: int) -> bool:
	if radio.get_status() == RadioStream.STATUS_ERROR:
		print("RESULT  FAIL (%s)" % radio.get_last_error())
		quit(1)
		return true
	if rate > 0 and radio.get_frames_available() >= int(rate * PREBUFFER_SECONDS):
		started = true
		print("prebuffered in %.2fs -- %d Hz, %d ch" % [elapsed, rate, radio.get_channels()])
		return false
	if elapsed > CONNECT_TIMEOUT:
		print("RESULT  FAIL (no audio after %.0fs; status=%d)" % [elapsed, radio.get_status()])
		quit(1)
		return true
	return false


func _report(rate: int) -> bool:
	var expected := int(play_time * rate)
	var drift := 0.0
	if expected > 0:
		drift = 100.0 * (float(consumed) - float(expected)) / float(expected)
	var kbps := (float(radio.get_bytes_received()) * 8.0 / 1000.0) / elapsed

	print("")
	print("ran              %.1fs (%.1fs playing, %.0f fps)" % [
		elapsed, play_time, float(frames) / maxf(play_time, 0.001)])
	print("decode rate      %.1f%% of real-time" % [
		100.0 * (float(radio.get_frames_decoded()) / float(rate)) / elapsed])
	print("input backlog    %d bytes" % radio.get_input_backlog())
	print("format           %d Hz, %d ch" % [rate, radio.get_channels()])
	print("network          %.1f KiB @ %.0f kbps" % [radio.get_bytes_received() / 1024.0, kbps])
	print("decoded          %d frames (%d mp3 frames)" % [
		radio.get_frames_decoded(), radio.get_mp3_frames()])
	# Accounting: every received byte should end up either decoded or skipped.
	var audio_bytes := radio.get_bytes_received() - radio.get_skipped_bytes() - radio.get_input_backlog()
	print("byte accounting  %d recv = %d audio + %d skipped + %d backlog" % [
		radio.get_bytes_received(), audio_bytes,
		radio.get_skipped_bytes(), radio.get_input_backlog()])
	print("consumed         %d frames (%.2f%% vs real-time)" % [consumed, drift])
	print("fifo at end      %.2fs" % (float(radio.get_frames_available()) / float(rate)))
	var late_trims := radio.get_buffer_trims() - maxi(steady_trims, 0)
	print("buffer trims     %d total, %d after settling" % [radio.get_buffer_trims(), late_trims])
	print("STARVATIONS      %d" % starvations)

	var ok := starvations == 0 and late_trims == 0 and absf(drift) < 1.0
	print("RESULT           %s" % ("PASS" if ok else "FAIL"))
	radio.close()
	quit(0 if ok else 1)
	return true
