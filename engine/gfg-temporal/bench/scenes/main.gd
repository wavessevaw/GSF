## GFG Temporal Engine quality bench.  One procedural scene per run, chosen
## with `-- --scene=<name>`; motion is a pure function of the frame index, so
## a Movie Maker run at --fixed-fps 90 is reproducible ground truth.
##
## Scenes stress what breaks frame generators on handhelds:
##   pan        fast camera yaw (stick flick), pillars, checker floor, HUD
##   sword      thin metallic blade swinging in front of a textured wall
##   specular   glossy spheres and floor, orbiting camera and moving light
##   particles  sparks, alpha smoke, a moving glass panel
##   walk       forward/strafe walk through thin posts (parallax, disocclusion)
##   orbit      third-person: character fixed on screen, world orbits behind
##   carry      the same with a sword on the character's back: a thin object
##              that stays still on screen while the background pans behind it
extends Node3D

const FPS := 90.0

var scene := "pan"
var frame := 0
var cam: Camera3D
var sword: Node3D
var light: OmniLight3D
var glass: MeshInstance3D
var hero: Node3D
var hud_dot: ColorRect
var hud_timer: Label
var hud_hp: ColorRect


func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--scene="):
			scene = a.trim_prefix("--scene=")
	seed(7)
	_world()
	_hud()
	call("_setup_" + scene)
	_update()


func _process(_d: float) -> void:
	frame += 1
	_update()


func _update() -> void:
	var t := frame / FPS
	call("_tick_" + scene, t)
	hud_timer.text = "%02d:%05.2f" % [int(t / 60.0), fmod(t, 60.0)]
	hud_dot.position = Vector2(80, 80) + Vector2(cos(t * 2.0), sin(t * 2.0)) * 40.0
	hud_hp.size.x = 300.0 * (0.6 + 0.3 * sin(t * 1.3))


# ---------------------------------------------------------------- materials

func _checker(n: int, a: Color, b: Color) -> ImageTexture:
	var img := Image.create(256, 256, true, Image.FORMAT_RGB8)
	for y in 256:
		for x in 256:
			img.set_pixel(x, y, a if ((x * n / 256) + (y * n / 256)) % 2 == 0 else b)
	img.generate_mipmaps()
	return ImageTexture.create_from_image(img)


func _noise(freq: float, s: int) -> ImageTexture:
	var fn := FastNoiseLite.new()
	fn.seed = s
	fn.frequency = freq
	fn.fractal_octaves = 5
	var img := fn.get_seamless_image(512, 512)
	img.convert(Image.FORMAT_RGB8)
	img.adjust_bcs(0.85, 1.25, 1.0)  # mid-grey detail for motion search
	img.generate_mipmaps()
	return ImageTexture.create_from_image(img)


func _mat(albedo: Texture2D, color := Color.WHITE, metal := 0.0, rough := 0.8, uv := Vector3.ONE) -> StandardMaterial3D:
	var m := StandardMaterial3D.new()
	m.albedo_texture = albedo
	m.albedo_color = color
	m.metallic = metal
	m.roughness = rough
	m.uv1_scale = uv
	m.texture_filter = BaseMaterial3D.TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC
	return m


func _box(size: Vector3, pos: Vector3, m: Material, parent: Node = self) -> MeshInstance3D:
	var mi := MeshInstance3D.new()
	var bm := BoxMesh.new()
	bm.size = size
	mi.mesh = bm
	mi.material_override = m
	mi.position = pos
	parent.add_child(mi)
	return mi


func _sphere(r: float, pos: Vector3, m: Material) -> MeshInstance3D:
	var mi := MeshInstance3D.new()
	var sm := SphereMesh.new()
	sm.radius = r
	sm.height = 2 * r
	sm.radial_segments = 48
	sm.rings = 24
	mi.mesh = sm
	mi.material_override = m
	mi.position = pos
	add_child(mi)
	return mi


# ---------------------------------------------------------------- world

func _world() -> void:
	var env := Environment.new()
	env.background_mode = Environment.BG_SKY
	var sky := Sky.new()
	var sm := ProceduralSkyMaterial.new()
	sm.sky_top_color = Color(0.25, 0.42, 0.75)
	sm.sky_horizon_color = Color(0.7, 0.75, 0.82)
	sky.sky_material = sm
	env.sky = sky
	env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	env.reflected_light_source = Environment.REFLECTION_SOURCE_SKY
	env.tonemap_mode = Environment.TONE_MAPPER_FILMIC
	env.tonemap_exposure = 0.75
	env.ambient_light_energy = 0.6
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-50, 35, 0)
	sun.light_energy = 0.9
	sun.shadow_enabled = true
	add_child(sun)
	cam = Camera3D.new()
	cam.fov = 70
	add_child(cam)
	var floor_m := _mat(_checker(16, Color(0.55, 0.55, 0.52), Color(0.25, 0.26, 0.28)), Color.WHITE, 0.0, 0.6, Vector3(30, 30, 1))
	_box(Vector3(120, 0.2, 120), Vector3(0, -0.1, 0), floor_m)


func _pillars(n: int, area: float, thin := false) -> void:
	var stone := _mat(_noise(0.02, 3), Color.WHITE, 0.0, 0.9, Vector3(1, 3, 1))
	var paint := _mat(_noise(0.05, 9), Color(0.8, 0.3, 0.25), 0.0, 0.5)
	for i in n:
		var p := Vector3(randf_range(-area, area), 0, randf_range(-area, area))
		if p.length() < 3.0:
			continue
		var w := randf_range(0.03, 0.08) if thin else randf_range(0.4, 1.4)
		var h := randf_range(1.5, 6.0)
		_box(Vector3(w, h, w if thin else randf_range(0.4, 1.4)), p + Vector3(0, h / 2, 0), stone if i % 3 else paint)


func _hud() -> void:
	var cl := CanvasLayer.new()
	add_child(cl)
	var panel := ColorRect.new()  # semi-transparent minimap backing
	panel.color = Color(0, 0, 0, 0.45)
	panel.position = Vector2(20, 20)
	panel.size = Vector2(120, 120)
	cl.add_child(panel)
	hud_dot = ColorRect.new()
	hud_dot.color = Color(1, 0.85, 0.2)
	hud_dot.size = Vector2(8, 8)
	cl.add_child(hud_dot)
	var hp_back := ColorRect.new()
	hp_back.color = Color(0, 0, 0, 0.5)
	hp_back.position = Vector2(40, 740)
	hp_back.size = Vector2(304, 22)
	cl.add_child(hp_back)
	hud_hp = ColorRect.new()
	hud_hp.color = Color(0.85, 0.15, 0.15)
	hud_hp.position = Vector2(42, 742)
	hud_hp.size = Vector2(300, 18)
	cl.add_child(hud_hp)
	var name_l := Label.new()
	name_l.text = "QUEST: Reach the old watchtower"
	name_l.position = Vector2(860, 30)
	name_l.add_theme_font_size_override("font_size", 22)
	name_l.add_theme_color_override("font_outline_color", Color.BLACK)
	name_l.add_theme_constant_override("outline_size", 4)
	cl.add_child(name_l)
	hud_timer = Label.new()
	hud_timer.position = Vector2(1120, 740)
	hud_timer.add_theme_font_size_override("font_size", 26)
	cl.add_child(hud_timer)
	for d in [Vector2(-12, -1), Vector2(4, -1), Vector2(-1, -12), Vector2(-1, 4)]:
		var c := ColorRect.new()  # crosshair
		c.color = Color(1, 1, 1, 0.9)
		c.position = Vector2(640, 400) + d
		c.size = Vector2(8, 2) if abs(d.x) > 2 else Vector2(2, 8)
		cl.add_child(c)


# ---------------------------------------------------------------- scenes

func _setup_pan() -> void:
	_pillars(160, 40)


func _tick_pan(t: float) -> void:
	# stick flick: yaw rate 240 deg/s * sin(2 pi t / 1.6), i.e. ramps up, reverses
	var u: float = clamp(t, 0.0, 1.6)
	var yaw := 240.0 * 1.6 / TAU * (1.0 - cos(TAU * u / 1.6))
	cam.position = Vector3(0, 1.7, 0)
	cam.rotation_degrees = Vector3(-4.0 + 3.0 * sin(t * 3.0), yaw, 0)


func _setup_sword() -> void:
	_pillars(60, 25)
	var wall := _mat(_noise(0.03, 5), Color.WHITE, 0.0, 0.85, Vector3(4, 2, 1))
	_box(Vector3(14, 6, 0.4), Vector3(0, 3, -6), wall)
	sword = Node3D.new()
	add_child(sword)
	var steel := _mat(null, Color(0.85, 0.87, 0.9), 1.0, 0.18)
	var leather := _mat(_noise(0.2, 11), Color(0.4, 0.25, 0.15), 0.0, 0.7)
	var gold := _mat(null, Color(0.9, 0.7, 0.3), 1.0, 0.25)
	_box(Vector3(0.035, 0.95, 0.008), Vector3(0, 0.62, 0), steel, sword)   # blade
	_box(Vector3(0.24, 0.025, 0.03), Vector3(0, 0.13, 0), gold, sword)     # crossguard
	_box(Vector3(0.03, 0.2, 0.03), Vector3(0, 0.02, 0), leather, sword)    # grip (hilt)
	_box(Vector3(0.05, 0.05, 0.05), Vector3(0, -0.09, 0), gold, sword)     # pommel


func _tick_sword(t: float) -> void:
	cam.position = Vector3(0.6 * sin(t * 0.7), 1.6, 1.2)
	cam.rotation_degrees = Vector3(-3, 8.0 * sin(t * 0.5), 0)
	# two swings per second, wrist roll, swept across the view
	var ph := t * TAU * 1.0
	sword.position = Vector3(0.25 * sin(ph), 1.25 + 0.1 * cos(ph * 2.0), -0.2)
	sword.rotation = Vector3(-0.4 + 0.35 * cos(ph * 2.0), 0.6 * sin(ph), 1.4 * sin(ph))


func _setup_specular() -> void:
	for i in 9:
		var m := _mat(null, Color.from_hsv(i / 9.0, 0.5, 0.9), 0.6 if i % 2 else 0.0, 0.08 + 0.05 * (i % 3))
		_sphere(0.6, Vector3((i % 3 - 1) * 1.6, 0.6, (i / 3 - 1) * 1.6), m)
	var plate := _mat(_noise(0.4, 2), Color(0.8, 0.8, 0.85), 1.0, 0.12, Vector3(4, 4, 1))
	_box(Vector3(8, 0.05, 8), Vector3(0, 0.02, 0), plate)
	light = OmniLight3D.new()
	light.omni_range = 12
	light.light_energy = 3.0
	light.light_color = Color(1, 0.9, 0.75)
	add_child(light)


func _tick_specular(t: float) -> void:
	var a := t * 0.9
	cam.position = Vector3(6.0 * sin(a), 2.6, 6.0 * cos(a))
	cam.look_at(Vector3(0, 0.5, 0))
	light.position = Vector3(3.0 * cos(t * 2.3), 2.0, 3.0 * sin(t * 2.3))


func _setup_particles() -> void:
	_pillars(70, 25)
	var sparks := CPUParticles3D.new()
	sparks.amount = 400
	sparks.lifetime = 0.8
	sparks.emission_shape = CPUParticles3D.EMISSION_SHAPE_SPHERE
	sparks.emission_sphere_radius = 0.2
	sparks.direction = Vector3(0, 1, 0)
	sparks.spread = 60
	sparks.initial_velocity_min = 3
	sparks.initial_velocity_max = 6
	sparks.gravity = Vector3(0, -9.8, 0)
	var qm := QuadMesh.new()
	qm.size = Vector2(0.03, 0.03)
	var em := StandardMaterial3D.new()
	em.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	em.albedo_color = Color(1, 0.7, 0.2)
	em.billboard_mode = BaseMaterial3D.BILLBOARD_ENABLED
	qm.material = em
	sparks.mesh = qm
	sparks.position = Vector3(0.5, 0.8, -3)
	add_child(sparks)
	var smoke := CPUParticles3D.new()
	smoke.amount = 40
	smoke.lifetime = 3.0
	smoke.direction = Vector3(0, 1, 0)
	smoke.initial_velocity_min = 0.4
	smoke.initial_velocity_max = 0.8
	smoke.gravity = Vector3.ZERO
	smoke.scale_amount_min = 1.0
	smoke.scale_amount_max = 2.0
	var sq := QuadMesh.new()
	var sm := StandardMaterial3D.new()
	sm.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	sm.albedo_color = Color(0.6, 0.6, 0.62, 0.25)
	sm.billboard_mode = BaseMaterial3D.BILLBOARD_ENABLED
	sm.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	sq.material = sm
	smoke.mesh = sq
	smoke.position = Vector3(-1.5, 0.5, -4)
	add_child(smoke)
	var gm := StandardMaterial3D.new()
	gm.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	gm.albedo_color = Color(0.6, 0.85, 1.0, 0.35)
	gm.metallic = 0.3
	gm.roughness = 0.05
	glass = _box(Vector3(1.4, 2.0, 0.03), Vector3(0, 1.2, -2), gm)


func _tick_particles(t: float) -> void:
	cam.position = Vector3(0, 1.6, 1.0)
	cam.rotation_degrees = Vector3(-6, 25.0 * sin(t * 1.4), 0)
	glass.position.x = 1.5 * sin(t * 2.2)


func _setup_walk() -> void:
	_pillars(70, 30)
	var wood := _mat(_noise(0.3, 4), Color(0.55, 0.4, 0.25), 0.0, 0.8)
	for i in 120:  # fence posts: thin, many depths
		var z := -2.0 - i * 0.45
		for s in [-1.6, 1.6]:
			_box(Vector3(0.05, 1.3, 0.05), Vector3(s + randf_range(-0.2, 0.2), 0.65, z), wood)
	for i in 40:  # rails
		_box(Vector3(0.03, 0.03, 2.0), Vector3(-1.6, 1.0, -3.0 - i * 2.0), wood)


func _tick_walk(t: float) -> void:
	cam.position = Vector3(0.9 * sin(t * 2.0), 1.6 + 0.04 * sin(t * 12.0), 4.0 - 6.0 * t)
	cam.rotation_degrees = Vector3(-2, 30.0 * sin(t * 1.1), 0)


func _setup_orbit() -> void:
	_pillars(140, 35)
	hero = Node3D.new()
	add_child(hero)
	var cloth := _mat(_noise(0.15, 13), Color(0.3, 0.35, 0.6), 0.0, 0.7)
	var skin := _mat(null, Color(0.85, 0.65, 0.5), 0.0, 0.6)
	var body := MeshInstance3D.new()
	var cm := CapsuleMesh.new()
	cm.radius = 0.28
	cm.height = 1.3
	body.mesh = cm
	body.material_override = cloth
	body.position = Vector3(0, 0.95, 0)
	hero.add_child(body)
	var head := MeshInstance3D.new()
	var hs := SphereMesh.new()
	hs.radius = 0.16
	hs.height = 0.32
	head.mesh = hs
	head.material_override = skin
	head.position = Vector3(0, 1.8, 0)
	hero.add_child(head)


func _tick_orbit(t: float) -> void:
	# camera orbits the hero (stick yaw 150 deg/s at peak); hero stays centred
	var yaw := deg_to_rad(150.0) * (t - 0.6 * sin(t * 2.0) / 2.0)
	hero.position = Vector3(0, 0, -2.0 * t)
	hero.rotation.y = yaw + PI
	var off := Vector3(sin(yaw), 0, cos(yaw)) * 3.2
	cam.position = hero.position + off + Vector3(0.5 * cos(yaw), 1.9, -0.5 * sin(yaw))
	cam.look_at(hero.position + Vector3(0, 1.5, 0))


func _setup_carry() -> void:
	_setup_orbit()
	# sword on the back, diagonal over the right shoulder, parented to the hero
	var steel := _mat(null, Color(0.85, 0.87, 0.9), 1.0, 0.18)
	var leather := _mat(_noise(0.2, 11), Color(0.4, 0.25, 0.15), 0.0, 0.7)
	var gold := _mat(null, Color(0.9, 0.7, 0.3), 1.0, 0.25)
	var back := Node3D.new()
	back.position = Vector3(0.05, 1.25, -0.32)
	back.rotation = Vector3(0, 0, -0.6)
	hero.add_child(back)
	_box(Vector3(0.035, 0.95, 0.008), Vector3(0, -0.45, 0), steel, back)  # blade, pointing down
	_box(Vector3(0.24, 0.025, 0.03), Vector3(0, 0.05, 0), gold, back)     # crossguard
	_box(Vector3(0.03, 0.22, 0.03), Vector3(0, 0.18, 0), leather, back)   # hilt above the shoulder
	_box(Vector3(0.05, 0.05, 0.05), Vector3(0, 0.31, 0), gold, back)      # pommel


func _tick_carry(t: float) -> void:
	_tick_orbit(t)
