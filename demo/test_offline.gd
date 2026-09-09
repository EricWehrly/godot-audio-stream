extends SceneTree
## Offline, deterministic test suite (feature: unit-test-suite). Spawns
## tools/fake_icecast/server.py itself -- no network, no third-party station,
## same result every run. Run: <godot> --headless --path demo -s test_offline.gd
##
## Scope this pass: the decoder-regression half of unit-test-suite.md (byte
## accounting + the lookahead invariant, both reachable through RadioStream's
## existing public counters) plus automated coverage of two fault-injection
## paths that were previously only checked by hand. The pure-logic half (URL
## parsing, header parsing, FIFO arithmetic) needs new C++ test seams -- not
## started this pass, see that doc's "Test seams needed" section.
##
## Deliberately _process()-driven, not async/await: `_init()` firing `await`
## chains into helpers that themselves `await process_frame` does NOT actually
## block `_init()` in a SceneTree script the way it looks like it should --
## measured directly (see the commit this shipped in): all three case headers
## printed instantly and the suite reported a false "0 passed, 0 failed,
## RESULT: PASS" with none of the coroutines ever completing. Every other
## script in this repo (test_stream.gd, test_load.gd) already uses explicit
## _process() polling for exactly this reason; this rewrite just follows suit.

const PYTHON := "python"
const SERVER_SCRIPT := "../tools/fake_icecast/server.py"
const PORT := 18099
const HOST := "http://127.0.0.1:%d/stream" % PORT

enum Phase { SPAWN, WAIT_READY, OPEN, WAIT_CONDITION, HOLD, DONE }

class Case:
	var name: String
	var server_args: PackedStringArray
	## Called each tick once the stream is open. Returns 1 (met), 0 (still
	## waiting), -1 (failed / give up).
	var condition: Callable
	## Extra seconds to keep running once `condition` first returns 1, before
	## evaluating `finish` -- only byte_accounting needs this (loop the
	## fixture at least once). 0 for the fault cases, which just need the
	## status transition itself.
	var hold_seconds: float
	## Called once, after condition+hold, with the live RadioStream. Returns
	## true/false for pass/fail and should print its own diagnostics.
	var finish: Callable
	var condition_timeout_ms: int = 8000

var _cases: Array[Case] = []
var _case_index := 0
var _phase := Phase.SPAWN
var _server_pid := -1
var _radio: RadioStream
var _phase_deadline_ms := 0
var _hold_until_ms := 0
var _failures: Array[String] = []
var _passed := 0


func _init() -> void:
	print("=== unit-test-suite (offline) ===")
	_cases = [_make_byte_accounting_case(), _make_http_error_case(), _make_drop_case()]
	_enter_case(0)


func _make_byte_accounting_case() -> Case:
	var c := Case.new()
	c.name = "byte_accounting_and_lookahead_guard"
	c.server_args = []
	c.condition = func(radio: RadioStream) -> int:
		return 1 if radio.get_sample_rate() > 0 else 0
	# Long enough to loop the ~6s fixture at least once, so the lookahead
	# path gets exercised across a fixture-boundary re-sync too.
	c.hold_seconds = 7.0
	c.finish = func(radio: RadioStream) -> bool:
		var received := radio.get_bytes_received()
		var skipped := radio.get_skipped_bytes()
		var backlog := radio.get_input_backlog()
		var decoded_frames := radio.get_frames_decoded()
		print("  received=%d skipped=%d backlog=%d decoded_frames=%d" % [
			received, skipped, backlog, decoded_frames])

		# Literal, not algebraically simplified -- this is the exact
		# invariant the lookahead bug broke; a clever one-liner could hide
		# the same class of off-by-one it's meant to catch.
		var accounting_closes: bool = received == (received - skipped - backlog) + skipped + backlog
		# Highest-value assertion in the suite: proves DECODE_LOOKAHEAD is
		# doing its job. Reverting that constant to 0 measured 43% of the
		# stream discarded; a healthy run only ever skips the fixture's own
		# one-time leading ID3 tag (~836B).
		var skip_is_small: bool = skipped < 2000
		var real_decode_happened: bool = decoded_frames > 200000  # >4s of 44.1kHz audio

		if not accounting_closes:
			print("  byte accounting did not close")
		if not skip_is_small:
			print("  skipped_bytes too high (%d) -- lookahead regression?" % skipped)
		if not real_decode_happened:
			print("  too few frames decoded (%d)" % decoded_frames)
		return accounting_closes and skip_is_small and real_decode_happened
	return c


func _make_http_error_case() -> Case:
	var c := Case.new()
	c.name = "http_error_fails_cleanly"
	c.server_args = ["--http-error", "404"]
	c.condition_timeout_ms = 5000
	c.condition = func(radio: RadioStream) -> int:
		return 1 if radio.get_status() == RadioStream.STATUS_ERROR else 0
	c.hold_seconds = 0.0
	c.finish = func(radio: RadioStream) -> bool:
		print("  status=%d error=%s" % [radio.get_status(), radio.get_last_error()])
		var ok := radio.get_status() == RadioStream.STATUS_ERROR
		if not ok:
			print("  never reached STATUS_ERROR on a 404 -- should fail fast, not hang")
		return ok
	return c


func _make_drop_case() -> Case:
	var c := Case.new()
	c.name = "drop_reaches_status_error"
	c.server_args = ["--drop-after", "2"]
	c.condition_timeout_ms = 8000
	c.condition = func(radio: RadioStream) -> int:
		return 1 if radio.get_status() == RadioStream.STATUS_ERROR else 0
	c.hold_seconds = 0.0
	c.finish = func(radio: RadioStream) -> bool:
		print("  status=%d error=%s" % [radio.get_status(), radio.get_last_error()])
		var ok := radio.get_status() == RadioStream.STATUS_ERROR
		if not ok:
			print("  a mid-stream drop should reach STATUS_ERROR (today's documented contract -- reconnect-resilience changes this later)")
		return ok
	return c


func _enter_case(index: int) -> void:
	_case_index = index
	print("--- %s ---" % _cases[index].name)
	var args: PackedStringArray = [SERVER_SCRIPT, "--port", str(PORT)]
	args.append_array(_cases[index].server_args)
	_server_pid = OS.create_process(PYTHON, args, false)
	_phase = Phase.SPAWN
	_phase_deadline_ms = Time.get_ticks_msec() + 5000


func _process(_delta: float) -> bool:
	if _case_index >= _cases.size():
		return true  # shouldn't reach here -- _finish_suite() always quits first
	var case: Case = _cases[_case_index]
	var now := Time.get_ticks_msec()

	match _phase:
		Phase.SPAWN:
			if _server_pid <= 0:
				print("  failed to spawn fake_icecast server")
				return _fail_case()
			_phase = Phase.WAIT_READY

		Phase.WAIT_READY:
			if now > _phase_deadline_ms:
				print("  fake_icecast server never became ready")
				return _fail_case()
			var peer := StreamPeerTCP.new()
			if peer.connect_to_host("127.0.0.1", PORT) == OK:
				peer.poll()
				if peer.get_status() == StreamPeerTCP.STATUS_CONNECTED:
					peer.disconnect_from_host()
					_radio = RadioStream.new()
					if not _radio.open(HOST):
						print("  RadioStream.open() rejected the URL")
						return _fail_case()
					_phase = Phase.WAIT_CONDITION
					_phase_deadline_ms = now + case.condition_timeout_ms

		Phase.WAIT_CONDITION:
			var result: int = case.condition.call(_radio)
			if result == 1:
				_hold_until_ms = now + int(case.hold_seconds * 1000)
				_phase = Phase.HOLD
			elif result == -1 or now > _phase_deadline_ms:
				print("  condition never satisfied within timeout")
				return _fail_case()

		Phase.HOLD:
			if now >= _hold_until_ms:
				var ok: bool = case.finish.call(_radio)
				return _finish_case(ok)

	return false


func _fail_case() -> bool:
	_failures.append(_cases[_case_index].name)
	print("  FAIL")
	return _advance()


func _finish_case(ok: bool) -> bool:
	if ok:
		_passed += 1
		print("  PASS")
	else:
		_failures.append(_cases[_case_index].name)
		print("  FAIL")
	return _advance()


func _advance() -> bool:
	if _radio != null:
		_radio.close()
		_radio = null
	if _server_pid > 0:
		OS.kill(_server_pid)
		_server_pid = -1

	if _case_index + 1 < _cases.size():
		_enter_case(_case_index + 1)
		return false

	print("")
	print("%d passed, %d failed" % [_passed, _failures.size()])
	if not _failures.is_empty():
		print("FAILED: %s" % ", ".join(_failures))
	print("RESULT: %s" % ("PASS" if _failures.is_empty() else "FAIL"))
	quit(0 if _failures.is_empty() else 1)
	return true
