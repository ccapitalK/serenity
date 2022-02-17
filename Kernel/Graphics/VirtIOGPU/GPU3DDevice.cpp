/*
 * Copyright (c) 2021, Sahan Fernando <sahan.h.fernando@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibC/sys/ioctl_numbers.h>
#include <Kernel/Graphics/GraphicsManagement.h>
#include <Kernel/Graphics/VirtIOGPU/Console.h>
#include <Kernel/Graphics/VirtIOGPU/GPU3DDevice.h>
#include <Kernel/Graphics/VirtIOGPU/GraphicsAdapter.h>
#include <Kernel/Graphics/VirtIOGPU/Protocol.h>
#include <Kernel/Random.h>

namespace Kernel::Graphics::VirtIOGPU {

ObjectHandle GPU3DDevice::allocate_object_handle() {
    m_object_handle_counter = m_object_handle_counter.value() + 1;
    return m_object_handle_counter;
}

[[maybe_unused]] static const char* frag_shader =
//      "FRAG\n"
//      "DCL IN[0], COLOR, COLOR\n"
//      "DCL IN[1], POSITION\n"
//      "DCL OUT[0], COLOR\n"
//      "DCL OUT[1], POSITION\n"
//      "  0: MOV OUT[0], IN[0]\n"
//      "  1: MOV OUT[1], IN[1]\n"
//      "  2: END\n";
    "FRAG\n"
    "PROPERTY FS_COLOR0_WRITES_ALL_CBUFS 1\n"
    "DCL IN[0], COLOR, COLOR\n"
    "DCL OUT[0], COLOR\n"
    "  0: MOV OUT[0], IN[0]\n"
    "  1: END\n";

[[maybe_unused]] static const char* vert_shader =
        "VERT\n"
        "DCL IN[0]\n"
        "DCL IN[1]\n"
        "DCL OUT[0], POSITION\n"
        "DCL OUT[1], COLOR\n"
        "DCL CONST[0..3]\n"
        "DCL TEMP[0..1]\n"
        "  0: MUL TEMP[0], IN[0].xxxx, CONST[0]\n"
        "  1: MAD TEMP[1], IN[0].yyyy, CONST[1], TEMP[0]\n"
        "  2: MAD TEMP[0], IN[0].zzzz, CONST[2], TEMP[1]\n"
        "  3: MAD OUT[0], IN[0].wwww, CONST[3], TEMP[0]\n"
        "  4: MOV_SAT OUT[1], IN[1]\n"
        "  5: END\n";

const struct FloatLookup {
    constexpr FloatLookup() {
        for (auto i = 0; i < 256; ++i) {
            valsf[i] = i/255.f;
        }
    }
    union {
        u32 vals[256];
        float valsf[256];
    };
} float_lookup;

static u32 encode_command(u32 length, u32 mid, VirGLCommand command) {
    u32 command_value = to_underlying(command);
    return (length << 16) | ((mid & 0xff) << 8) | (command_value & 0xff);
};

static float const_buffer[16] = {
    1, 0, 0, 0,
    0, 1, 0, 0,
    0, 0, 1, 0,
    0, 0, 0, 1
};

[[maybe_unused]] static void encode_set_constant_buffer(u32* data, size_t& used, size_t max) {
    size_t num_entries = sizeof(const_buffer)/sizeof(*const_buffer);
    VERIFY(used + num_entries + 3 <= max);
    data[used + 0] = encode_command(num_entries + 2, 0, VirGLCommand::SET_CONSTANT_BUFFER);
    data[used + 1] = AK::to_underlying(Protocol::Gallium::ShaderType::SHADER_VERTEX); // shader_type
    data[used + 2] = 0; // index (currently unused according to virglrenderer source code)
    for (size_t i = 0; i < num_entries; ++i) {
        data[used + 3 + i] = AK::bit_cast<u32>(const_buffer[i]);
    }
    used += num_entries + 3;
}

[[maybe_unused]] static void encode_create_subcontext(u32* data, size_t& used, size_t max, u32 subcontext) {
    VERIFY(used + 2 <= max);
    data[used + 0] = encode_command(1, 0, VirGLCommand::CREATE_SUB_CTX);
    data[used + 1] = subcontext;
    used += 2;
}

[[maybe_unused]] static void encode_create_blend(u32* data, size_t& used, size_t max, ObjectHandle handle) {
    VERIFY(used + 12 <= max);
    data[used + 0] = encode_command(11, to_underlying(Protocol::ObjectType::BLEND), VirGLCommand::CREATE_OBJECT);
    data[used + 1] = handle.value();
    data[used + 2] = 4; // Enable dither flag, and nothing else
    data[used + 3] = 0;
    data[used + 4] = 0x78000000; // Enable all bits of color mask for color buffer 0, and nothing else
    for (size_t i = 1; i < 8; ++i) {
        data[used + 4 + i] = 0; // Explicitly disable all flags for other color buffers
    }
    used += 12;
}

[[maybe_unused]] static void encode_bind_blend(u32* data, size_t& used, size_t max, ObjectHandle handle) {
    VERIFY(used + 3 <= max);
    data[used + 0] = encode_command(1, AK::to_underlying(Protocol::ObjectType::BLEND), VirGLCommand::BIND_OBJECT);
    data[used + 1] = handle.value(); // VIRGL_OBJ_BIND_HANDLE
    used += 2;
}

[[maybe_unused]] static void encode_set_tweaks(u32* data, size_t& used, size_t max, u32 id, u32 value) {
    VERIFY(used + 3 <= max);
    data[used + 0] = encode_command(2, 0, VirGLCommand::SET_TWEAKS);
    data[used + 1] = id;
    data[used + 2] = value;
    used += 3;
}

[[maybe_unused]] static void encode_set_polygon_stipple(u32* data, size_t& used, size_t max) {
    VERIFY(used + 33 <= max);
    data[used + 0] = encode_command(32, 0, VirGLCommand::SET_POLYGON_STIPPLE);
    for (auto i = 0u; i < 32u; ++i) {
        data[used + 1 + i] = 0xffffffffu;
    }
    used += 33;
}

[[maybe_unused]] static void encode_set_subcontext(u32* data, size_t& used, size_t max, u32 subcontext) {
    VERIFY(used + 2 <= max);
    data[used + 0] = encode_command(1, 0, VirGLCommand::SET_SUB_CTX);
    data[used + 1] = subcontext;
    used += 2;
}

[[maybe_unused]] static void encode_set_tess_state(u32* data, size_t& used, size_t max) {
    VERIFY(used + 7 <= max);
    data[used + 0] = encode_command(6, 0, VirGLCommand::SET_TESS_STATE);
    data[used + 1] = bit_cast<u32>((float)1.0f);
    data[used + 2] = bit_cast<u32>((float)1.0f);
    data[used + 3] = bit_cast<u32>((float)1.0f);
    data[used + 4] = bit_cast<u32>((float)1.0f);
    data[used + 5] = bit_cast<u32>((float)1.0f);
    data[used + 6] = bit_cast<u32>((float)1.0f);
    used += 7;
}

[[maybe_unused]] static void encode_create_shader(u32* data, size_t& used, size_t max, ObjectHandle handle, Protocol::Gallium::ShaderType shader_type, const char* shader_data) {
    size_t shader_len = strlen(shader_data) + 1; // Need to remember to copy null terminator as well if needed
    size_t params_length_in_words = 5 + ((shader_len+3)/4);
    VERIFY(used + params_length_in_words + 1 <= max);
    data[used + 0] = encode_command(params_length_in_words, to_underlying(Protocol::ObjectType::SHADER), VirGLCommand::CREATE_OBJECT);
    data[used + 1] = handle.value(); // VIRGL_OBJ_CREATE_HANDLE
    data[used + 2] = to_underlying(shader_type);
    data[used + 3] = 0; // VIRGL_OBJ_SHADER_OFFSET
    data[used + 4] = shader_len;
    data[used + 5] = 0; // VIRGL_OBJ_SHADER_NUM_TOKENS
    memcpy(&data[used + 6], shader_data, shader_len);
    used += params_length_in_words + 1;
}

[[maybe_unused]] static void encode_bind_shader(u32* data, size_t& used, size_t max, ObjectHandle handle, Protocol::Gallium::ShaderType shader_type) {
    VERIFY(used + 3 <= max);
    data[used + 0] = encode_command(2, 0, VirGLCommand::BIND_SHADER);
    data[used + 1] = handle.value(); // VIRGL_OBJ_BIND_HANDLE
    data[used + 2] = to_underlying(shader_type);
    used += 3;
}

static void encode_gl_clear(u32* data, size_t& used, size_t max, u8 r, u8 g, u8 b) {
    VERIFY(used + 9 <= max);
    data[used + 0] = encode_command(8, 0, VirGLCommand::CLEAR);
    data[used + 1] = 4;
    data[used + 2] = float_lookup.vals[r];
    data[used + 3] = float_lookup.vals[g];
    data[used + 4] = float_lookup.vals[b];
    data[used + 5] = float_lookup.vals[255];
    data[used + 8] = 0;
    u64 *depth = (u64*)(&data[used + 6]);
    *depth = AK::bit_cast<u64>((double)1.0f);
    used += 9;
}

[[maybe_unused]] static void encode_set_vertex_buffers(u32* data, size_t& used, size_t max, u32 stride, u32 offset, ResourceID resource) {
    VERIFY(used + 4 <= max);
    data[used + 0] = encode_command(3, 0, VirGLCommand::SET_VERTEX_BUFFERS);
    data[used + 1] = stride;
    data[used + 2] = offset;
    data[used + 3] = resource.value();
    used += 4;
}

[[maybe_unused]] static void encode_gl_viewport(u32* data, size_t& used, size_t max) {
    constexpr float width = 1024;
    constexpr float height = 768;
    VERIFY(used + 8 <= max);
    data[used + 0] = encode_command(7, 0, VirGLCommand::SET_VIEWPORT_STATE);
    data[used + 1] = 0;
    data[used + 2] = bit_cast<u32>(width/2); // scale_x
    data[used + 3] = bit_cast<u32>(height/2); // scale_y
    data[used + 4] = bit_cast<u32>(0.5f); // scale_z
    data[used + 5] = bit_cast<u32>(width/2); // translate_x
    data[used + 6] = bit_cast<u32>(height/2); // translate_y
    data[used + 7] = bit_cast<u32>(0.5f); // translate_z
    used += 8;
}

[[maybe_unused]] static void encode_transfer3d_flat(u32* data, size_t& used, size_t max, ResourceID resource, size_t length) {
    constexpr size_t cmd_len = 13;
    VERIFY(used + cmd_len + 1 <= max);
    data[used + 0] = encode_command(cmd_len, 0, VirGLCommand::TRANSFER3D);
    data[used + 1] = resource.value(); // res_handle
    data[used + 2] = 0; // level
    data[used + 3] = 242; // usage
    data[used + 4] = 0; // stride
    data[used + 5] = 0; // layer_stride
    data[used + 6] = 0; // x
    data[used + 7] = 0; // y
    data[used + 8] = 0; // z
    data[used + 9] = length; // width
    data[used + 10] = 1; // height
    data[used + 11] = 1; // depth
    data[used + 12] = 0; // data_offset
    data[used + 13] = 1; // direction
    used += cmd_len + 1;
}

[[maybe_unused]] static void encode_transfer3d_2d_res(u32* data, size_t& used, size_t max, ResourceID resource, Protocol::Rect dirty_rect) {
    constexpr size_t cmd_len = 13;
    VERIFY(used + cmd_len + 1 <= max);
    data[used + 0] = encode_command(cmd_len, 0, VirGLCommand::TRANSFER3D);
    data[used + 1] = resource.value(); // res_handle
    data[used + 2] = 0; // level
    data[used + 3] = 242; // usage
    data[used + 4] = 0; // stride
    data[used + 5] = 0; // layer_stride
    data[used + 6] = dirty_rect.x; // x
    data[used + 7] = dirty_rect.y; // y
    data[used + 8] = 0; // z
    data[used + 9] = dirty_rect.width; // width
    data[used + 10] = dirty_rect.height; // height
    data[used + 11] = 1; // depth
    data[used + 12] = 0; // data_offset
    data[used + 13] = 1; // direction
    used += cmd_len + 1;
}

[[maybe_unused]] static void encode_end_transfers_3d(u32* data, size_t& used, size_t max) {
    VERIFY(used + 1 <= max);
    data[used] = encode_command(0, 0, VirGLCommand::END_TRANSFERS);
    used += 1;
}

[[maybe_unused]] static void encode_draw_vbo(u32* data, size_t& used, size_t max) {
    constexpr size_t cmd_len = 12;
    VERIFY(used + cmd_len + 1 <= max);
    data[used + 0] = encode_command(cmd_len, 0, VirGLCommand::DRAW_VBO);
    data[used + 1] = 0; // start
    data[used + 2] = 3; // count
    data[used + 3] = AK::to_underlying(Protocol::PipePrimitiveTypes::TRIANGLES); // mode
    data[used + 4] = 0; // indexed
    data[used + 5] = 1; // instance_count
    data[used + 6] = 0; // index_bias
    data[used + 7] = 0; // start_instance
    data[used + 8] = 0; // primitive_restart
    data[used + 9] = 0; // restart_index
    data[used + 10] = 0; // min_index
    data[used + 11] = 0xffffffff; // max_index
    data[used + 12] = 0; // cso
    used += cmd_len + 1;
}

[[maybe_unused]] static void encode_create_vertex_elements(u32* data, size_t& used, size_t max, ObjectHandle handle) {
    constexpr size_t cmd_len = 9;
    VERIFY(used + cmd_len + 1 <= max);
    data[used + 0] = encode_command(cmd_len, AK::to_underlying(Protocol::ObjectType::VERTEX_ELEMENTS), VirGLCommand::CREATE_OBJECT);
    data[used + 1] = handle.value();
    data[used + 2] = 12; // src_offset_0
    data[used + 3] = 0;  // instance_divisor_0
    data[used + 4] = 0;  // vertex_buffer_index_0
    data[used + 5] = 29; // src_format_0 (PIPE_FORMAT_R32G32_FLOAT = 29)
    data[used + 6] = 0; // src_offset_1
    data[used + 7] = 0;  // instance_divisor_1
    data[used + 8] = 0;  // vertex_buffer_index_1
    data[used + 9] = 30; // src_format_1 (PIPE_FORMAT_R32G32B32_FLOAT = 30)
    used += cmd_len + 1;
}

[[maybe_unused]] static void encode_bind_vertex_elements(u32* data, size_t& used, size_t max, ObjectHandle handle) {
    VERIFY(used + 3 <= max);
    data[used + 0] = encode_command(1, AK::to_underlying(Protocol::ObjectType::VERTEX_ELEMENTS), VirGLCommand::BIND_OBJECT);
    data[used + 1] = handle.value(); // VIRGL_OBJ_BIND_HANDLE
    used += 2;
}

struct Vertex {
    union {
        struct {
            float r;
            float g;
            float b;
            float x;
            float y;
        };
        struct {
            u32 ru32;
            u32 gu32;
            u32 bu32;
            u32 xu32;
            u32 yu32;
        };
    };
};

static Vertex vertices[3] = {
    {.r = 1, .g = 0, .b = 0, .x = -0.8, .y = -0.8 },
    {.r = 0, .g = 1, .b = 0, .x = 0.8, .y = -0.8 },
    {.r = 0, .g = 0, .b = 1, .x = 0.0, .y = 0.9 },
};

// FIXME: This should signal if it fails
void GPU3DDevice::register_scanout_framebuffer(ResourceID resource_id)
{
    VERIFY(!m_scanout_handles.contains(resource_id));
    auto handle = allocate_object_handle();
    MUST(m_scanout_handles.try_set(resource_id, handle));
    m_graphics_adapter.attach_resource_to_context(resource_id, m_kernel_context_id);
    m_graphics_adapter.submit_command_buffer(m_kernel_context_id, [&](Bytes buffer) {
        auto* data = (u32*)buffer.data();
        size_t used = 0;
        // Create main surface
        data[used + 0] = encode_command(5, to_underlying(Protocol::ObjectType::SURFACE), VirGLCommand::CREATE_OBJECT);
        data[used + 1] = handle.value();
        data[used + 2] = resource_id.value();
        data[used + 3] = to_underlying(m_graphics_adapter.get_framebuffer_format());
        data[used + 4] = 0;
        data[used + 5] = 0;
        used += 6;
        // Set framebuffer backend
        data[used + 0] = encode_command(3, 0, VirGLCommand::SET_FRAMEBUFFER_STATE);
        data[used + 1] = 1; // nr_cbufs
        data[used + 2] = 0; // zsurf_handle
        data[used + 3] = handle.value(); // surf_handle
        used += 4;
        // Set framebuffer size
        constexpr u32 width = 1024;
        constexpr u32 height = 768;
        data[used + 0] = encode_command(2, 0, VirGLCommand::SET_FRAMEBUFFER_STATE_NO_ATTACH);
        data[used + 1] = (height << 16) | width; // (height << 16 | width)
        data[used + 2] = 0; // (samples << 16 | layers)
        used += 3;
        return used * sizeof(u32);
    });
}

void GPU3DDevice::unregister_scanout_framebuffer(ResourceID resource_id)
{
    VERIFY(m_scanout_handles.contains(resource_id));
    VERIFY(m_scanout_handles.remove(resource_id));
}

GPU3DDevice::GPU3DDevice(GraphicsAdapter& graphics_adapter)
    : CharacterDevice(28, 0)
    , m_graphics_adapter(graphics_adapter)
{
    m_kernel_context_id = m_graphics_adapter.create_context();
    dbgln("Got context id {}", m_kernel_context_id.value());

    // Setup memory transfer region
    {
        auto region_result = MM.allocate_kernel_region(TRANSFER_REGION_PAGES * PAGE_SIZE, "VIRGL3D upload buffer", Memory::Region::Access::ReadWrite);
        VERIFY(!region_result.is_error());
        m_transfer_buffer_region = region_result.release_value();
    }
}

void GPU3DDevice::setup_demo(Kernel::Graphics::VirtIOGPU::FramebufferDevice& framebuffer_device) {
    m_drawtarget_rect = {
        0,
        0,
        (u32)framebuffer_device.width(),
        (u32)framebuffer_device.height()
    };
    m_drawtarget_resource_id = framebuffer_device.get_main_resource_id();
    memcpy(m_transfer_buffer_region->vaddr().as_ptr(), vertices, sizeof(vertices));

    [[maybe_unused]] auto ve_handle = allocate_object_handle();
    [[maybe_unused]] auto blend_handle = allocate_object_handle();

    m_drawtarget_surface_handle = allocate_object_handle();
    m_blend_handle = allocate_object_handle();
    [[maybe_unused]] auto frag_shader_handle = allocate_object_handle();
    [[maybe_unused]] auto vert_shader_handle = allocate_object_handle();

    m_graphics_adapter.submit_command_buffer(m_kernel_context_id, [&](Bytes buffer) {
        auto* data = (u32*)buffer.data();
        size_t used = 0;
        // Create and bind a blend, to control the default color mask
        encode_create_blend(data, used, buffer.size(), m_blend_handle);
        encode_bind_blend(data, used, buffer.size(), m_blend_handle);
        // Set tweaks, I don't know whether we really need these
        // GLES: Apply dest swizzle when a BGRA surface is emulated by an RGBA surface
        // encode_set_tweaks(data, used, buffer.size(), 1, 1);
        // GLES: Value to return when emulating GL_SAMPLES_PASSES by using GL_ANY_SAMPLES_PASSES = 1024
        // encode_set_tweaks(data, used, buffer.size(), 2, 1024);
        // Set the polygon stipple to all 1s
        // encode_set_polygon_stipple(data, used, buffer.size());
        // Create main surface
        data[used + 0] = encode_command(5, to_underlying(Protocol::ObjectType::SURFACE), VirGLCommand::CREATE_OBJECT);
        data[used + 1] = m_drawtarget_surface_handle.value();
        data[used + 2] = m_drawtarget_resource_id.value();
        data[used + 3] = to_underlying(m_graphics_adapter.get_framebuffer_format());
        data[used + 4] = 0;
        data[used + 5] = 0;
        used += 6;
        // Set framebuffer backend
        data[used + 0] = encode_command(3, 0, VirGLCommand::SET_FRAMEBUFFER_STATE);
        data[used + 1] = 1; // nr_cbufs
        data[used + 2] = 0; // zsurf_handle
        data[used + 3] = m_drawtarget_surface_handle.value(); // surf_handle
        used += 4;
        // Set framebuffer size
        constexpr u32 width = 1024;
        constexpr u32 height = 768;
        data[used + 0] = encode_command(2, 0, VirGLCommand::SET_FRAMEBUFFER_STATE_NO_ATTACH);
        data[used + 1] = (height << 16) | width; // (height << 16 | width)
        data[used + 2] = 0; // (samples << 16 | layers)
        used += 3;
        // Create Fragment shader
        encode_create_shader(data, used, buffer.size(), frag_shader_handle, Protocol::Gallium::ShaderType::SHADER_FRAGMENT, frag_shader);
        encode_bind_shader(data, used, buffer.size(), frag_shader_handle, Protocol::Gallium::ShaderType::SHADER_FRAGMENT);
        // Create Vertex shader
        encode_create_shader(data, used, buffer.size(), vert_shader_handle, Protocol::Gallium::ShaderType::SHADER_VERTEX, vert_shader);
        encode_bind_shader(data, used, buffer.size(), vert_shader_handle, Protocol::Gallium::ShaderType::SHADER_VERTEX);
        // Set the viewport
        encode_gl_viewport(data, used, buffer.size());
        // Set the tess state
        // encode_set_tess_state(data, used, buffer.size());
        // Clear the framebuffer
        encode_gl_clear(data, used, buffer.size(), 0, 0, 0);
        // Create the vertex elements object
        encode_create_vertex_elements(data, used, buffer.size(), ve_handle);
        encode_bind_vertex_elements(data, used, buffer.size(), ve_handle);
        // Set the constant buffer (currently just stores the identity matrix)
        encode_set_constant_buffer(data, used, buffer.size());
        return used * sizeof(u32);
    });
}

ErrorOr<void> GPU3DDevice::ioctl(OpenFileDescription&, unsigned request, Userspace<void*> arg) {
    switch (request) {
    case VIRGL_IOCTL_SETUP_DEMO: {
        auto &framebuffer = *m_graphics_adapter.m_scanouts[0].framebuffer;
        framebuffer.deactivate_writes();
        MutexLocker locker(m_graphics_adapter.operation_lock());
        setup_demo(framebuffer);
        return {};
    }
    case VIRGL_IOCTL_TRANSFER_DATA: {
        auto user_transfer_descriptor = static_ptr_cast<VirGLTransferDescriptor const*>(arg);
        auto transfer_descriptor = TRY(copy_typed_from_user(user_transfer_descriptor));
        if (transfer_descriptor.direction == VIRGL_DATA_DIR_GUEST_TO_HOST) {
            if (transfer_descriptor.num_bytes > TRANSFER_REGION_PAGES * PAGE_SIZE) {
                return EOVERFLOW;
            }
            auto target = m_transfer_buffer_region->vaddr().offset(transfer_descriptor.offset_in_region).as_ptr();
            return copy_from_user(target, transfer_descriptor.data, transfer_descriptor.num_bytes);
        } else {
            return EINVAL;
        }
    }
    case VIRGL_IOCTL_SUBMIT_CMD: {
        MutexLocker locker(m_graphics_adapter.operation_lock());
        auto user_command_buffer = static_ptr_cast<VirGLCommandBuffer const*>(arg);
        auto command_buffer = TRY(copy_typed_from_user(user_command_buffer));
        m_graphics_adapter.submit_command_buffer(m_kernel_context_id, [&](Bytes buffer) {
            auto num_bytes = command_buffer.num_elems * sizeof(u32);
            VERIFY( num_bytes <= buffer.size());
            MUST(copy_from_user(buffer.data(), command_buffer.data, num_bytes));
            return num_bytes;
        });
        return {};
    }
    case VIRGL_IOCTL_FLUSH_DISPLAY: {
        MutexLocker locker(m_graphics_adapter.operation_lock());
        m_graphics_adapter.flush_displayed_image(m_drawtarget_resource_id, m_drawtarget_rect);
        return {};
    }
    case VIRGL_IOCTL_CREATE_RESOURCE: {
        auto user_spec = static_ptr_cast<VirGL3DResourceSpec const*>(arg);
        VirGL3DResourceSpec spec = TRY(copy_typed_from_user(user_spec));

        Protocol::Resource3DSpecification const resource_spec = {
            .target = static_cast<Protocol::Gallium::PipeTextureTarget>(spec.target),
            .format = spec.format,
            .bind = spec.bind,
            .width = spec.width,
            .height = spec.height,
            .depth = spec.depth,
            .array_size = spec.array_size,
            .last_level = spec.last_level,
            .nr_samples = spec.nr_samples,
            .flags = spec.flags
        };
        MutexLocker locker(m_graphics_adapter.operation_lock());
        auto resource_id = m_graphics_adapter.create_3d_resource(resource_spec).value();
        m_graphics_adapter.attach_resource_to_context(resource_id, m_kernel_context_id);
        m_graphics_adapter.ensure_backing_storage(resource_id, *m_transfer_buffer_region, 0, TRANSFER_REGION_PAGES * PAGE_SIZE);
        dbgln("Created resource with ID: {}", resource_id);
        spec.created_resource_id = resource_id;
        // FIXME: We should delete the resource we just created if we fail to copy the resource id out
        return copy_to_user(static_ptr_cast<VirGL3DResourceSpec*>(arg), &spec);
    }
    case VIRGL_IOCTL_FINISH_DEMO: {
        auto &framebuffer = *m_graphics_adapter.m_scanouts[0].framebuffer;
        framebuffer.activate_writes();
        return {};
    }
    }
    return EINVAL;
}

void GPU3DDevice::bind_shader(const char*)
{
}

void GPU3DDevice::transfer_scanout(ResourceID scanout_resource, Protocol::Rect dirty_rect) {
    // TODO: Finish this
    (void) dirty_rect;
    // auto handle = m_scanout_handles.get(scanout_resouce);
    m_graphics_adapter.submit_command_buffer(m_kernel_context_id, [&](Bytes buffer) {
        auto* data = (u32*)buffer.data();
        size_t used = 0;
        // Transfer data to vbo
        encode_transfer3d_2d_res(data, used, buffer.size(), scanout_resource, dirty_rect);
        encode_end_transfers_3d(data, used, buffer.size());
        return used * sizeof(u32);
    });
}

}
