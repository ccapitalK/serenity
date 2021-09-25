/*
 * Copyright (c) 2021, Sahan Fernando <sahan.h.fernando@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Kernel/Graphics/VirtIOGPU/Console.h>
#include <Kernel/Graphics/VirtIOGPU/GPU3DDevice.h>
#include <Kernel/Graphics/VirtIOGPU/Protocol.h>
#include <Kernel/Random.h>

namespace Kernel::Graphics::VirtIOGPU {

static const char* frag_shader =
        "FRAG\n"
        "PROPERTY FS_COLOR0_WRITES_ALL_CBUFS 1\n"
        "DCL IN[0], COLOR, COLOR\n"
        "DCL OUT[0], COLOR\n"
        "  0: MOV OUT[0], IN[0]\n"
        "  1: END\n";
static const char* vert_shader =
        "VERT\n"
        "DCL IN[0]\n"
        "DCL IN[1]\n"
        "DCL OUT[0], POSITION\n"
        "DCL OUT[1], COLOR\n"
        "DCL CONST[0..3]\n"
        "DCL CONST[0]\n"
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

static void encode_create_shader(u32*data, size_t& used, size_t max, ObjectHandle handle, Protocol::GalliumShaderType shader_type, const char* shader_data) {
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

static void encode_bind_shader(u32*data, size_t& used, size_t max, ObjectHandle handle, Protocol::GalliumShaderType shader_type) {
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
    data[used + 6] = 1;
    data[used + 7] = 0;
    data[used + 8] = 0;
    used += 9;
}

static void encode_set_vertex_buffers(u32* data, size_t& used, size_t max, u32 stride, u32 offset, ResourceID handle) {
    VERIFY(used + 4 <= max);
    data[used + 0] = encode_command(3, 0, VirGLCommand::SET_VERTEX_BUFFERS);
    data[used + 1] = stride;
    data[used + 2] = offset;
    data[used + 3] = handle.value();
    used += 4;
}

static void encode_gl_viewport(u32* data, size_t& used, size_t max) {
    VERIFY(used + 8 <= max);
    data[used + 0] = encode_command(7, 0, VirGLCommand::SET_VIEWPORT_STATE);
    data[used + 1] = 0;
    data[used + 2] = bit_cast<u32>(1.f);
    data[used + 3] = bit_cast<u32>(1.f);
    data[used + 4] = bit_cast<u32>(1.f);
    data[used + 5] = bit_cast<u32>(0.f);
    data[used + 6] = bit_cast<u32>(0.f);
    data[used + 7] = bit_cast<u32>(0.f);
    used += 8;
}

static void encode_draw_vbo(u32* data, size_t& used, size_t max) {
    constexpr size_t cmd_len = 12;
    VERIFY(cmd_len + 1 <= max);
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

GPU3DDevice::GPU3DDevice(GPU& gpu, FrameBufferDevice &framebuffer_device)
    : m_virtio_gpu(gpu)
    , m_framebuffer_device(framebuffer_device)
{
    m_context_id = m_virtio_gpu.create_context();
    dbgln("Got context id {}", m_context_id.value());

    m_drawtarget_rect = {
        0,
        0,
        (u32)m_framebuffer_device.width(),
        (u32)m_framebuffer_device.height()
    };
    m_drawtarget_resource_id = m_virtio_gpu.create_2d_resource(m_drawtarget_rect);
    dbgln("Got drawtarget resource id {}", m_drawtarget_resource_id);
    m_virtio_gpu.attach_resource_to_context(m_drawtarget_resource_id, m_context_id);
    m_virtio_gpu.set_scanout_resource(m_framebuffer_device.m_scanout, m_drawtarget_resource_id, m_drawtarget_rect);

    GPU::Resource3DSpecification const vbo_spec = {
        .target = 0,
        .format = 67, // pipe_to_virgl_format
        .bind = VIRGL_BIND_VERTEX_BUFFER,
        .width = 128,
        .height = 1,
        .depth = 1,
        .array_size = 1,
        .last_level = 0,
        .nr_samples = 0,
        .flags = 0
    };
    m_vbo_resource_id = m_virtio_gpu.create_3d_resource(vbo_spec);
    dbgln("Got vbo resource id {}", m_vbo_resource_id);

    m_drawtarget_surface_handle = allocate_object_handle();
    auto frag_shader_handle = allocate_object_handle();
    auto vert_shader_handle = allocate_object_handle();

    m_virtio_gpu.submit_command_buffer(m_context_id, [&](Bytes buffer) {
        auto* data = (u32*)buffer.data();
        size_t used = 0;
        // Create surface
        data[used + 0] = encode_command(5, to_underlying(Protocol::ObjectType::SURFACE), VirGLCommand::CREATE_OBJECT);
        data[used + 1] = m_drawtarget_surface_handle.value();
        data[used + 2] = m_drawtarget_resource_id.value();
        data[used + 3] = to_underlying(m_virtio_gpu.get_framebuffer_format());
        data[used + 4] = 0;
        data[used + 5] = 0;
        used += 6;
        // Set framebuffer backend
        data[used + 0] = encode_command(3, 0, VirGLCommand::SET_FRAMEBUFFER_STATE);
        data[used + 1] = 1;
        data[used + 2] = 0;
        data[used + 3] = m_drawtarget_surface_handle.value();
        used += 4;
        // Create Fragment shader
        encode_create_shader(data, used, buffer.size(), frag_shader_handle, Protocol::GalliumShaderType::SHADER_FRAGMENT, frag_shader);
        encode_bind_shader(data, used, buffer.size(), frag_shader_handle, Protocol::GalliumShaderType::SHADER_FRAGMENT);
        // Create Vertex shader
        encode_create_shader(data, used, buffer.size(), vert_shader_handle, Protocol::GalliumShaderType::SHADER_VERTEX, vert_shader);
        encode_bind_shader(data, used, buffer.size(), vert_shader_handle, Protocol::GalliumShaderType::SHADER_VERTEX);
        // Set the viewport
        encode_gl_viewport(data, used, buffer.size());
        // Clear the framebuffer
        encode_gl_clear(data, used, buffer.size(), 255, 0, 0);
        encode_set_vertex_buffers(data, used, buffer.size(), 20, 0, m_vbo_resource_id);
        encode_draw_vbo(data, used, buffer.size());
        return used * sizeof(u32);
    });
    m_virtio_gpu.flush_resource(m_drawtarget_resource_id, m_drawtarget_rect);
    VERIFY_NOT_REACHED();
    while (1) {
        draw_frame();
        IO::delay(200000);
    }
    VERIFY_NOT_REACHED();
    // Create VBO
    // Set foreground color to white
    // Upload VBO data
    // Create VAO
    // Call DrawVBO
    // Flush
}

ResourceID GPU3DDevice::create_and_upload_resource(u8*)
{
    TODO();
}

void GPU3DDevice::draw_frame()
{
    // TODO: Upload shader, needed to draw anything
    m_virtio_gpu.submit_command_buffer(m_context_id, [&](Bytes buffer) {
        auto* data = (u32*)buffer.data();
        size_t used = 0;
        // Clear the framebuffer
        auto r = get_fast_random<u8>();
        auto g = get_fast_random<u8>();
        auto b = get_fast_random<u8>();
        encode_gl_clear(data, used, buffer.size(), r, g, b);
        if (0) {
            encode_draw_vbo(data, used, buffer.size());
        }
        return used * sizeof(u32);
    });
    m_virtio_gpu.flush_resource(m_drawtarget_resource_id, m_drawtarget_rect);
}

void GPU3DDevice::bind_shader(const char*)
{

}

}
