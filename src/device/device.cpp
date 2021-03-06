/*
 * Copyright (c) 2012-2020 Daniele Bartolini and individual contributors.
 * License: https://github.com/dbartolini/crown/blob/master/LICENSE
 */

#include "config.h"
#include "core/containers/array.inl"
#include "core/filesystem/file.h"
#include "core/filesystem/filesystem.h"
#include "core/filesystem/filesystem_apk.h"
#include "core/filesystem/filesystem_disk.h"
#include "core/filesystem/path.h"
#include "core/json/json_object.inl"
#include "core/json/sjson.h"
#include "core/list.inl"
#include "core/math/constants.h"
#include "core/math/matrix4x4.inl"
#include "core/math/vector3.inl"
#include "core/memory/globals.h"
#include "core/memory/proxy_allocator.h"
#include "core/memory/temp_allocator.inl"
#include "core/os.h"
#include "core/strings/dynamic_string.inl"
#include "core/strings/string.inl"
#include "core/strings/string_id.inl"
#include "core/strings/string_stream.inl"
#include "core/time.h"
#include "core/types.h"
#include "device/console_server.h"
#include "device/device.h"
#include "device/input_device.h"
#include "device/input_manager.h"
#include "device/log.h"
#include "device/pipeline.h"
#include "device/profiler.h"
#include "lua/lua_environment.h"
#include "lua/lua_stack.inl"
#include "resource/config_resource.h"
#include "resource/font_resource.h"
#include "resource/level_resource.h"
#include "resource/lua_resource.h"
#include "resource/material_resource.h"
#include "resource/mesh_resource.h"
#include "resource/package_resource.h"
#include "resource/physics_resource.h"
#include "resource/resource_id.h"
#include "resource/resource_loader.h"
#include "resource/resource_manager.h"
#include "resource/resource_package.h"
#include "resource/shader_resource.h"
#include "resource/sound_resource.h"
#include "resource/sprite_resource.h"
#include "resource/state_machine_resource.h"
#include "resource/texture_resource.h"
#include "resource/unit_resource.h"
#include "world/audio.h"
#include "world/material_manager.h"
#include "world/physics.h"
#include "world/shader_manager.h"
#include "world/unit_manager.h"
#include "world/world.h"
#include <bgfx/bgfx.h>
#include <bx/allocator.h>
#include <bx/math.h>

#define MAX_SUBSYSTEMS_HEAP 8 * 1024 * 1024

LOG_SYSTEM(DEVICE, "device")

namespace crown
{
#if CROWN_TOOLS
extern void tool_init(void);
extern void tool_update(float);
extern void tool_shutdown(void);
extern bool tool_process_events();
#endif

extern bool next_event(OsEvent& ev);

struct BgfxCallback : public bgfx::CallbackI
{
	virtual void fatal(const char* _filePath, uint16_t _line, bgfx::Fatal::Enum _code, const char* _str)
	{
		CE_ASSERT(false, "Fatal error: 0x%08x: %s", _code, _str);
		CE_UNUSED(_filePath);
		CE_UNUSED(_line);
		CE_UNUSED(_code);
		CE_UNUSED(_str);
	}

	virtual void traceVargs(const char* /*_filePath*/, u16 /*_line*/, const char* _format, va_list _argList)
	{
		char buf[2048];
		strncpy(buf, _format, sizeof(buf)-1);
		buf[strlen32(buf)-1] = '\0'; // Remove trailing newline
		vlogi(DEVICE, buf, _argList);
	}

	virtual void profilerBegin(const char* /*_name*/, uint32_t /*_abgr*/, const char* /*_filePath*/, uint16_t /*_line*/)
	{
	}

	virtual void profilerBeginLiteral(const char* /*_name*/, uint32_t /*_abgr*/, const char* /*_filePath*/, uint16_t /*_line*/)
	{
	}

	virtual void profilerEnd()
	{
	}

	virtual u32 cacheReadSize(u64 /*_id*/)
	{
		return 0;
	}

	virtual bool cacheRead(u64 /*_id*/, void* /*_data*/, u32 /*_size*/)
	{
		return false;
	}

	virtual void cacheWrite(u64 /*_id*/, const void* /*_data*/, u32 /*_size*/)
	{
	}

	virtual void screenShot(const char* /*_filePath*/, u32 /*_width*/, u32 /*_height*/, u32 /*_pitch*/, const void* /*_data*/, u32 /*_size*/, bool /*_yflip*/)
	{
	}

	virtual void captureBegin(u32 /*_width*/, u32 /*_height*/, u32 /*_pitch*/, bgfx::TextureFormat::Enum /*_format*/, bool /*_yflip*/)
	{
	}

	virtual void captureEnd()
	{
	}

	virtual void captureFrame(const void* /*_data*/, u32 /*_size*/)
	{
	}
};

struct BgfxAllocator : public bx::AllocatorI
{
	ProxyAllocator _allocator;

	BgfxAllocator(Allocator& a)
		: _allocator(a, "bgfx")
	{
	}

	virtual void* realloc(void* _ptr, size_t _size, size_t _align, const char* /*_file*/, u32 /*_line*/)
	{
		if (!_ptr)
			return _allocator.allocate((u32)_size, (u32)_align == 0 ? 16 : (u32)_align);

		if (_size == 0)
		{
			_allocator.deallocate(_ptr);
			return NULL;
		}

		// Realloc
		void* p = _allocator.allocate((u32)_size, (u32)_align == 0 ? 16 : (u32)_align);
		_allocator.deallocate(_ptr);
		return p;
	}
};

static void console_command_script(ConsoleServer& /*cs*/, TCPSocket& /*client*/, const char* json, void* user_data)
{
	TempAllocator4096 ta;
	JsonObject obj(ta);
	DynamicString script(ta);

	sjson::parse(obj, json);
	sjson::parse_string(script, obj["script"]);

	((Device*)user_data)->_lua_environment->execute_string(script.c_str());
}

static void console_command(ConsoleServer& cs, TCPSocket& client, const char* json, void* user_data)
{
	TempAllocator4096 ta;
	JsonObject obj(ta);
	JsonArray args(ta);

	sjson::parse(obj, json);
	sjson::parse_array(args, obj["args"]);

	DynamicString cmd(ta);
	sjson::parse_string(cmd, args[0]);

	if (cmd == "pause")
		device()->pause();
	else if (cmd == "unpause")
		device()->unpause();
	else if (cmd == "reload")
	{
		if (array::size(args) != 3)
		{
			cs.error(client, "Usage: reload type name");
			return;
		}

		DynamicString type(ta);
		DynamicString name(ta);
		sjson::parse_string(type, args[1]);
		sjson::parse_string(name, args[2]);

		((Device*)user_data)->reload(ResourceId(type.c_str()), ResourceId(name.c_str()));
	}
}

Device::Device(const DeviceOptions& opts, ConsoleServer& cs)
	: _allocator(default_allocator(), MAX_SUBSYSTEMS_HEAP)
	, _options(opts)
	, _boot_config(default_allocator())
	, _console_server(&cs)
	, _data_filesystem(NULL)
	, _last_log(NULL)
	, _resource_loader(NULL)
	, _resource_manager(NULL)
	, _bgfx_allocator(NULL)
	, _bgfx_callback(NULL)
	, _shader_manager(NULL)
	, _material_manager(NULL)
	, _input_manager(NULL)
	, _unit_manager(NULL)
	, _lua_environment(NULL)
	, _pipeline(NULL)
	, _display(NULL)
	, _window(NULL)
	, _width(0)
	, _height(0)
	, _quit(false)
	, _paused(false)
{
	list::init_head(_worlds);
}

bool Device::process_events(bool vsync)
{
#if CROWN_TOOLS
	return tool_process_events();
#endif

	bool exit = false;
	bool reset = false;

	OsEvent event;
	while (next_event(event))
	{
		if (event.type == OsEventType::NONE)
			continue;

		switch (event.type)
		{
		case OsEventType::BUTTON:
		case OsEventType::AXIS:
		case OsEventType::STATUS:
			_input_manager->read(event);
			break;

		case OsEventType::RESOLUTION:
			_width  = event.resolution.width;
			_height = event.resolution.height;
			reset   = true;
			break;

		case OsEventType::EXIT:
			exit = true;
			break;

		case OsEventType::PAUSE:
			pause();
			break;

		case OsEventType::RESUME:
			unpause();
			break;

		case OsEventType::TEXT:
			break;

		default:
			CE_FATAL("Unknown OS event");
			break;
		}
	}

	if (reset)
		bgfx::reset(_width, _height, (vsync ? BGFX_RESET_VSYNC : BGFX_RESET_NONE));

	return exit;
}

void Device::run()
{
	s64 run_t0 = time::now();

	_console_server->register_command("command", console_command, this);

	_console_server->listen(_options._console_port, _options._wait_console);

#if CROWN_PLATFORM_ANDROID
	_data_filesystem = CE_NEW(_allocator, FilesystemApk)(default_allocator(), const_cast<AAssetManager*>((AAssetManager*)_options._asset_manager));
#else
	_data_filesystem = CE_NEW(_allocator, FilesystemDisk)(default_allocator());
	{
		char cwd[1024];
		const char* data_dir = !_options._data_dir.empty()
			? _options._data_dir.c_str()
			: os::getcwd(cwd, sizeof(cwd))
			;
		((FilesystemDisk*)_data_filesystem)->set_prefix(data_dir);
	}

	_last_log = _data_filesystem->open(CROWN_LAST_LOG, FileOpenMode::WRITE);
#endif // CROWN_PLATFORM_ANDROID

	logi(DEVICE, "Crown %s %s %s", CROWN_VERSION, CROWN_PLATFORM_NAME, CROWN_ARCH_NAME);

	profiler_globals::init();

	namespace smr = state_machine_internal;
	namespace cor = config_resource_internal;
	namespace ftr = font_resource_internal;
	namespace lur = lua_resource_internal;
	namespace lvr = level_resource_internal;
	namespace mhr = mesh_resource_internal;
	namespace mtr = material_resource_internal;
	namespace pcr = physics_config_resource_internal;
	namespace pkr = package_resource_internal;
	namespace sar = sprite_animation_resource_internal;
	namespace sdr = sound_resource_internal;
	namespace shr = shader_resource_internal;
	namespace spr = sprite_resource_internal;
	namespace txr = texture_resource_internal;
	namespace utr = unit_resource_internal;

	_resource_loader  = CE_NEW(_allocator, ResourceLoader)(*_data_filesystem);
	_resource_loader->register_fallback(RESOURCE_TYPE_TEXTURE,  StringId64("core/fallback/fallback"));
	_resource_loader->register_fallback(RESOURCE_TYPE_MATERIAL, StringId64("core/fallback/fallback"));
	_resource_loader->register_fallback(RESOURCE_TYPE_UNIT,     StringId64("core/fallback/fallback"));

	_resource_manager = CE_NEW(_allocator, ResourceManager)(*_resource_loader);
	_resource_manager->register_type(RESOURCE_TYPE_CONFIG,           RESOURCE_VERSION_CONFIG,           cor::load, cor::unload, NULL,        NULL        );
	_resource_manager->register_type(RESOURCE_TYPE_FONT,             RESOURCE_VERSION_FONT,             NULL,      NULL,        NULL,        NULL        );
	_resource_manager->register_type(RESOURCE_TYPE_LEVEL,            RESOURCE_VERSION_LEVEL,            NULL,      NULL,        NULL,        NULL        );
	_resource_manager->register_type(RESOURCE_TYPE_MATERIAL,         RESOURCE_VERSION_MATERIAL,         mtr::load, mtr::unload, mtr::online, mtr::offline);
	_resource_manager->register_type(RESOURCE_TYPE_MESH,             RESOURCE_VERSION_MESH,             mhr::load, mhr::unload, mhr::online, mhr::offline);
	_resource_manager->register_type(RESOURCE_TYPE_PACKAGE,          RESOURCE_VERSION_PACKAGE,          pkr::load, pkr::unload, NULL,        NULL        );
	_resource_manager->register_type(RESOURCE_TYPE_PHYSICS_CONFIG,   RESOURCE_VERSION_PHYSICS_CONFIG,   NULL,      NULL,        NULL,        NULL        );
	_resource_manager->register_type(RESOURCE_TYPE_SCRIPT,           RESOURCE_VERSION_SCRIPT,           NULL,      NULL,        NULL,        NULL        );
	_resource_manager->register_type(RESOURCE_TYPE_SHADER,           RESOURCE_VERSION_SHADER,           shr::load, shr::unload, shr::online, shr::offline);
	_resource_manager->register_type(RESOURCE_TYPE_SOUND,            RESOURCE_VERSION_SOUND,            NULL,      NULL,        NULL,        NULL        );
	_resource_manager->register_type(RESOURCE_TYPE_SPRITE,           RESOURCE_VERSION_SPRITE,           NULL,      NULL,        NULL,        NULL        );
	_resource_manager->register_type(RESOURCE_TYPE_SPRITE_ANIMATION, RESOURCE_VERSION_SPRITE_ANIMATION, NULL,      NULL,        NULL,        NULL        );
	_resource_manager->register_type(RESOURCE_TYPE_STATE_MACHINE,    RESOURCE_VERSION_STATE_MACHINE,    NULL,      NULL,        NULL,        NULL        );
	_resource_manager->register_type(RESOURCE_TYPE_TEXTURE,          RESOURCE_VERSION_TEXTURE,          txr::load, txr::unload, txr::online, txr::offline);
	_resource_manager->register_type(RESOURCE_TYPE_UNIT,             RESOURCE_VERSION_UNIT,             NULL,      NULL,        NULL,        NULL        );

	// Read config
	{
		TempAllocator512 ta;
		DynamicString boot_dir(ta);
		if (_options._boot_dir != NULL)
		{
			boot_dir += _options._boot_dir;
			boot_dir += '/';
		}
		boot_dir += CROWN_BOOT_CONFIG;

		const StringId64 config_name(boot_dir.c_str());
		_resource_manager->load(RESOURCE_TYPE_CONFIG, config_name);
		_resource_manager->flush();
		_boot_config.parse((const char*)_resource_manager->get(RESOURCE_TYPE_CONFIG, config_name));
		_resource_manager->unload(RESOURCE_TYPE_CONFIG, config_name);
	}

	// Init all remaining subsystems
	_display = display::create(_allocator);

	_width  = _boot_config.window_w;
	_height = _boot_config.window_h;

	_window = window::create(_allocator);
	_window->open(_options._window_x
		, _options._window_y
		, _width
		, _height
		, _options._parent_window
		);
	_window->set_title(_boot_config.window_title.c_str());
	_window->set_fullscreen(_boot_config.fullscreen);
	_window->bgfx_setup();

	_bgfx_allocator = CE_NEW(_allocator, BgfxAllocator)(default_allocator());
	_bgfx_callback  = CE_NEW(_allocator, BgfxCallback)();

	bgfx::Init init;
	init.resolution.width  = _width;
	init.resolution.height = _height;
	init.resolution.reset  = _boot_config.vsync ? BGFX_RESET_VSYNC : BGFX_RESET_NONE;
	init.callback  = _bgfx_callback;
	init.allocator = _bgfx_allocator;
	init.vendorId = BGFX_PCI_ID_NONE;
#if CROWN_PLATFORM_ANDROID
	init.type = bgfx::RendererType::OpenGLES;
#elif CROWN_PLATFORM_LINUX
	init.type = bgfx::RendererType::OpenGL;
#elif CROWN_PLATFORM_WINDOWS
	init.type = bgfx::RendererType::Direct3D11;
#else
	#error "Unknown platform"
#endif
	bgfx::init(init);

	_shader_manager   = CE_NEW(_allocator, ShaderManager)(default_allocator());
	_material_manager = CE_NEW(_allocator, MaterialManager)(default_allocator(), *_resource_manager);
	_input_manager    = CE_NEW(_allocator, InputManager)(default_allocator());
	_unit_manager     = CE_NEW(_allocator, UnitManager)(default_allocator());
	_lua_environment  = CE_NEW(_allocator, LuaEnvironment)();
	_lua_environment->register_console_commands(*_console_server);

	audio_globals::init();
	physics_globals::init(_allocator);

	ResourcePackage* boot_package = create_resource_package(_boot_config.boot_package_name);
	boot_package->load();
	boot_package->flush();

	_lua_environment->load_libs();
	_lua_environment->require(_boot_config.boot_script_name.c_str());
	_lua_environment->execute_string(_options._lua_string.c_str());

	_pipeline = CE_NEW(_allocator, Pipeline)();
	_pipeline->create(_width, _height);

#if CROWN_TOOLS
	tool_init();
#endif

	logi(DEVICE, "Initialized in %.2fs", time::seconds(time::now() - run_t0));

	_lua_environment->call_global("init");

	u16 old_width = _width;
	u16 old_height = _height;
	s64 time_last = time::now();

	while (!process_events(_boot_config.vsync) && !_quit)
	{
		const s64 time = time::now();
		const f32 dt   = f32(time::seconds(time - time_last));
		time_last = time;

		profiler_globals::clear();
		_console_server->update();

		RECORD_FLOAT("device.dt", dt);
		RECORD_FLOAT("device.fps", 1.0f/dt);

		if (_width != old_width || _height != old_height)
		{
			old_width = _width;
			old_height = _height;
			_pipeline->reset(_width, _height);
		}

		if (!_paused)
		{
			_resource_manager->complete_requests();

			{
				const s64 t0 = time::now();
				LuaStack stack(_lua_environment->L);
				stack.push_float(dt);
				_lua_environment->call_global("update", 1);
				RECORD_FLOAT("lua.update", f32(time::seconds(time::now() - t0)));
			}
			{
				const s64 t0 = time::now();
				LuaStack stack(_lua_environment->L);
				stack.push_float(dt);
				_lua_environment->call_global("render", 1);
				RECORD_FLOAT("lua.render", f32(time::seconds(time::now() - t0)));
			}
		}

		_lua_environment->reset_temporaries();
		_input_manager->update();

		const bgfx::Stats* stats = bgfx::getStats();
		RECORD_FLOAT("bgfx.gpu_time", f32(f64(stats->gpuTimeEnd - stats->gpuTimeBegin)/stats->gpuTimerFreq));
		RECORD_FLOAT("bgfx.cpu_time", f32(f64(stats->cpuTimeEnd - stats->cpuTimeBegin)/stats->cpuTimerFreq));

		profiler_globals::flush();

#if CROWN_TOOLS
		tool_update(dt);
#endif

		bgfx::frame();
	}

#if CROWN_TOOLS
	tool_shutdown();
#endif

	_lua_environment->call_global("shutdown");

	boot_package->unload();
	destroy_resource_package(*boot_package);

	physics_globals::shutdown(_allocator);
	audio_globals::shutdown();

	_pipeline->destroy();
	CE_DELETE(_allocator, _pipeline);
	CE_DELETE(_allocator, _lua_environment);
	CE_DELETE(_allocator, _unit_manager);
	CE_DELETE(_allocator, _input_manager);
	CE_DELETE(_allocator, _material_manager);
	CE_DELETE(_allocator, _shader_manager);
	CE_DELETE(_allocator, _resource_manager);
	CE_DELETE(_allocator, _resource_loader);

	bgfx::shutdown();
	CE_DELETE(_allocator, _bgfx_callback);
	CE_DELETE(_allocator, _bgfx_allocator);

	_window->close();
	window::destroy(_allocator, *_window);
	display::destroy(_allocator, *_display);

	if (_last_log)
		_data_filesystem->close(*_last_log);

	CE_DELETE(_allocator, _data_filesystem);

	profiler_globals::shutdown();

	_allocator.clear();
}

void Device::quit()
{
	_quit = true;
}

void Device::pause()
{
	_paused = true;
	logi(DEVICE, "Paused");
}

void Device::unpause()
{
	_paused = false;
	logi(DEVICE, "Unpaused");
}

void Device::resolution(u16& width, u16& height)
{
	width = _width;
	height = _height;
}

void Device::render(World& world, UnitId camera_unit)
{
	const f32 aspect_ratio = (_boot_config.aspect_ratio == -1.0f
		? (f32)_width/(f32)_height
		: _boot_config.aspect_ratio
		);
	world.camera_set_aspect(camera_unit, aspect_ratio);
	world.camera_set_viewport_metrics(camera_unit, 0, 0, _width, _height);

	const Matrix4x4 view = world.camera_view_matrix(camera_unit);
	const Matrix4x4 proj = world.camera_projection_matrix(camera_unit);

	const bgfx::Caps* caps = bgfx::getCaps();
	f32 bx_ortho[16];
	bx::mtxOrtho(bx_ortho, 0, _width, 0, _height, 0.0f, 1.0f, 0.0f, caps->homogeneousDepth);
	Matrix4x4 ortho_proj = from_array(bx_ortho);

	bgfx::setViewClear(VIEW_SPRITE_0, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x353839ff, 1.0f, 0);

	bgfx::setViewTransform(VIEW_SPRITE_0, to_float_ptr(view), to_float_ptr(proj));
	bgfx::setViewTransform(VIEW_SPRITE_1, to_float_ptr(view), to_float_ptr(proj));
	bgfx::setViewTransform(VIEW_SPRITE_2, to_float_ptr(view), to_float_ptr(proj));
	bgfx::setViewTransform(VIEW_SPRITE_3, to_float_ptr(view), to_float_ptr(proj));
	bgfx::setViewTransform(VIEW_SPRITE_4, to_float_ptr(view), to_float_ptr(proj));
	bgfx::setViewTransform(VIEW_SPRITE_5, to_float_ptr(view), to_float_ptr(proj));
	bgfx::setViewTransform(VIEW_SPRITE_6, to_float_ptr(view), to_float_ptr(proj));
	bgfx::setViewTransform(VIEW_SPRITE_7, to_float_ptr(view), to_float_ptr(proj));
	bgfx::setViewTransform(VIEW_MESH, to_float_ptr(view), to_float_ptr(proj));
	bgfx::setViewTransform(VIEW_DEBUG, to_float_ptr(view), to_float_ptr(proj));
	bgfx::setViewTransform(VIEW_GUI, to_float_ptr(MATRIX4X4_IDENTITY), to_float_ptr(ortho_proj));

	bgfx::setViewRect(VIEW_SPRITE_0, 0, 0, _width, _height);
	bgfx::setViewRect(VIEW_SPRITE_1, 0, 0, _width, _height);
	bgfx::setViewRect(VIEW_SPRITE_2, 0, 0, _width, _height);
	bgfx::setViewRect(VIEW_SPRITE_3, 0, 0, _width, _height);
	bgfx::setViewRect(VIEW_SPRITE_4, 0, 0, _width, _height);
	bgfx::setViewRect(VIEW_SPRITE_5, 0, 0, _width, _height);
	bgfx::setViewRect(VIEW_SPRITE_6, 0, 0, _width, _height);
	bgfx::setViewRect(VIEW_SPRITE_7, 0, 0, _width, _height);
	bgfx::setViewRect(VIEW_MESH, 0, 0, _width, _height);
	bgfx::setViewRect(VIEW_DEBUG, 0, 0, _width, _height);
	bgfx::setViewRect(VIEW_GUI, 0, 0, _width, _height);
	bgfx::setViewRect(VIEW_GRAPH, 0, 0, _width, _height);

	bgfx::setViewMode(VIEW_SPRITE_0, bgfx::ViewMode::DepthAscending);
	bgfx::setViewMode(VIEW_SPRITE_1, bgfx::ViewMode::DepthAscending);
	bgfx::setViewMode(VIEW_SPRITE_2, bgfx::ViewMode::DepthAscending);
	bgfx::setViewMode(VIEW_SPRITE_3, bgfx::ViewMode::DepthAscending);
	bgfx::setViewMode(VIEW_SPRITE_4, bgfx::ViewMode::DepthAscending);
	bgfx::setViewMode(VIEW_SPRITE_5, bgfx::ViewMode::DepthAscending);
	bgfx::setViewMode(VIEW_SPRITE_6, bgfx::ViewMode::DepthAscending);
	bgfx::setViewMode(VIEW_SPRITE_7, bgfx::ViewMode::DepthAscending);
	bgfx::setViewMode(VIEW_GUI, bgfx::ViewMode::Sequential);

	bgfx::setViewFrameBuffer(VIEW_SPRITE_0, _pipeline->_frame_buffer);
	bgfx::setViewFrameBuffer(VIEW_SPRITE_1, _pipeline->_frame_buffer);
	bgfx::setViewFrameBuffer(VIEW_SPRITE_2, _pipeline->_frame_buffer);
	bgfx::setViewFrameBuffer(VIEW_SPRITE_3, _pipeline->_frame_buffer);
	bgfx::setViewFrameBuffer(VIEW_SPRITE_4, _pipeline->_frame_buffer);
	bgfx::setViewFrameBuffer(VIEW_SPRITE_5, _pipeline->_frame_buffer);
	bgfx::setViewFrameBuffer(VIEW_SPRITE_6, _pipeline->_frame_buffer);
	bgfx::setViewFrameBuffer(VIEW_SPRITE_7, _pipeline->_frame_buffer);
	bgfx::setViewFrameBuffer(VIEW_MESH, _pipeline->_frame_buffer);
	bgfx::setViewFrameBuffer(VIEW_DEBUG, _pipeline->_frame_buffer);
	bgfx::setViewFrameBuffer(VIEW_GUI, _pipeline->_frame_buffer);
	bgfx::setViewFrameBuffer(VIEW_GRAPH, _pipeline->_frame_buffer);

	bgfx::touch(VIEW_SPRITE_0);
	bgfx::touch(VIEW_SPRITE_1);
	bgfx::touch(VIEW_SPRITE_2);
	bgfx::touch(VIEW_SPRITE_3);
	bgfx::touch(VIEW_SPRITE_4);
	bgfx::touch(VIEW_SPRITE_5);
	bgfx::touch(VIEW_SPRITE_6);
	bgfx::touch(VIEW_SPRITE_7);
	bgfx::touch(VIEW_MESH);
	bgfx::touch(VIEW_DEBUG);
	bgfx::touch(VIEW_GUI);
	bgfx::touch(VIEW_GRAPH);

	world.render(view);

#if !CROWN_TOOLS
	_pipeline->render(*_shader_manager, StringId32("blit"), 0, _width, _height);
#endif // CROWN_TOOLS
}

World* Device::create_world()
{
	World* world = CE_NEW(default_allocator(), World)(default_allocator()
		, *_resource_manager
		, *_shader_manager
		, *_material_manager
		, *_unit_manager
		, *_lua_environment
		);

	list::add(world->_node, _worlds);
	return world;
}

void Device::destroy_world(World& world)
{
	list::remove(world._node);
	CE_DELETE(default_allocator(), &world);
}

ResourcePackage* Device::create_resource_package(StringId64 id)
{
	return CE_NEW(default_allocator(), ResourcePackage)(id, *_resource_manager);
}

void Device::destroy_resource_package(ResourcePackage& rp)
{
	CE_DELETE(default_allocator(), &rp);
}

void Device::reload(StringId64 type, StringId64 name)
{
	ResourceId res_id = resource_id(type, name);

	logi(DEVICE, "Reloading: " RESOURCE_ID_FMT, res_id._id);

	_resource_manager->reload(type, name);
	const void* new_resource = _resource_manager->get(type, name);

	if (type == RESOURCE_TYPE_SCRIPT)
	{
		_lua_environment->execute((const LuaResource*)new_resource, 0);
	}

	logi(DEVICE, "Reloaded: " RESOURCE_ID_FMT, res_id._id);
}

void Device::log(const char* msg)
{
	if (_last_log)
	{
		_last_log->write(msg, strlen32(msg));
		_last_log->write("\n", 1);
		_last_log->flush();
	}
}

char _buffer[sizeof(Device)];
Device* _device = NULL;

void run(const DeviceOptions& opts)
{
	CE_ASSERT(_device == NULL, "Crown already initialized");
	console_server_globals::init();
	_device = new (_buffer) Device(opts, *console_server());
	_device->run();
	_device->~Device();
	_device = NULL;
	console_server_globals::shutdown();
}

Device* device()
{
	return crown::_device;
}

} // namespace crown
