extends SceneTree

## Headless check: does the extension load and register its class?
## Run: <godot> --headless --path demo -s test_load.gd
## Used to verify one api_version=4.6 build loads in BOTH Godot 4.6 and 4.7.


func _init() -> void:
	var engine_version: String = Engine.get_version_info()["string"]
	print("engine           %s" % engine_version)

	var registered := ClassDB.class_exists("RadioStream")
	print("class registered %s" % registered)
	if not registered:
		print("RESULT           FAIL (extension did not load)")
		quit(1)
		return

	var radio: Object = ClassDB.instantiate("RadioStream")
	print("instantiated     %s" % (radio != null))

	# Exercise the binding surface without touching the network.
	print("sample_rate      %d" % radio.get_sample_rate())
	print("status           %d" % radio.get_status())
	print("frames_available %d" % radio.get_frames_available())

	# A URL that cannot parse must fail cleanly rather than crash.
	var rejected: bool = not radio.open("ftp://nope")
	print("bad url rejected %s" % rejected)

	if not rejected:
		print("RESULT           FAIL (bad URL was accepted)")
		quit(1)
		return

	print("RESULT           PASS")
	quit(0)
