/*
* Copyright (c) 2022, Sahan Fernando <sahan.h.fernando@gmail.com>
*
* SPDX-License-Identifier: BSD-2-Clause
 */

#include "CommandBufferBuilder.h"
#include "VirGLProtocol.h"

static u32 encode_command(u32 length, u32 mid, Protocol::VirGLCommand command) {
    u32 command_value = to_underlying(command);
    return (length << 16) | ((mid & 0xff) << 8) | (command_value & 0xff);
};

void CommandBufferBuilder::append_transfer3d_flat(ResourceID resource, size_t length) {
    constexpr size_t cmd_len = 13;
    m_buffer.append(encode_command(cmd_len, 0, Protocol::VirGLCommand::TRANSFER3D));
    m_buffer.append(resource.value()); // res_handle
    m_buffer.append(0); // level
    m_buffer.append(242); // usage
    m_buffer.append(0); // stride
    m_buffer.append(0); // layer_stride
    m_buffer.append(0); // x
    m_buffer.append(0); // y
    m_buffer.append(0); // z
    m_buffer.append(length); // width
    m_buffer.append(1); // height
    m_buffer.append(1); // depth
    m_buffer.append(0); // data_offset
    m_buffer.append(1); // direction
}

void CommandBufferBuilder::append_end_transfers_3d() {
    m_buffer.append(encode_command(0, 0, Protocol::VirGLCommand::END_TRANSFERS));
}

void CommandBufferBuilder::append_draw_vbo(u32 count) {
    constexpr size_t cmd_len = 12;
    m_buffer.append(encode_command(cmd_len, 0, Protocol::VirGLCommand::DRAW_VBO));
    m_buffer.append(0); // start
    m_buffer.append(count); // count
    m_buffer.append(AK::to_underlying(Protocol::PipePrimitiveTypes::TRIANGLES)); // mode
    m_buffer.append(0); // indexed
    m_buffer.append(1); // instance_count
    m_buffer.append(0); // index_bias
    m_buffer.append(0); // start_instance
    m_buffer.append(0); // primitive_restart
    m_buffer.append(0); // restart_index
    m_buffer.append(0); // min_index
    m_buffer.append(0xffffffff); // max_index
    m_buffer.append(0); // cso
}

void CommandBufferBuilder::append_gl_clear(float r, float g, float b) {
    m_buffer.append(encode_command(8, 0, Protocol::VirGLCommand::CLEAR));
    m_buffer.append(4);
    m_buffer.append(bit_cast<u32>(r));
    m_buffer.append(bit_cast<u32>(g));
    m_buffer.append(bit_cast<u32>(b));
    m_buffer.append(bit_cast<u32>(1.0f));
    size_t depth_pos = m_buffer.size();
    m_buffer.append(0);
    m_buffer.append(0);
    m_buffer.append(0);
    u64 *depth = (u64*)(&m_buffer[depth_pos]);
    *depth = AK::bit_cast<u64>((double)1.0f);
}

void CommandBufferBuilder::append_set_vertex_buffers(u32 stride, u32 offset, ResourceID resource) {
    m_buffer.append(encode_command(3, 0, Protocol::VirGLCommand::SET_VERTEX_BUFFERS));
    m_buffer.append(stride);
    m_buffer.append(offset);
    m_buffer.append(resource.value());
}

void CommandBufferBuilder::append_create_blend(ObjectHandle handle) {
    m_buffer.append(encode_command(11, to_underlying(Protocol::ObjectType::BLEND), Protocol::VirGLCommand::CREATE_OBJECT));
    m_buffer.append(handle.value());
    m_buffer.append(4); // Enable dither flag, and nothing else
    m_buffer.append(0);
    m_buffer.append(0x78000000); // Enable all bits of color mask for color buffer 0, and nothing else
    for (size_t i = 1; i < 8; ++i) {
        m_buffer.append(0); // Explicitly disable all flags for other color buffers
    }
}

void CommandBufferBuilder::append_bind_blend(ObjectHandle handle) {
    m_buffer.append(encode_command(1, AK::to_underlying(Protocol::ObjectType::BLEND), Protocol::VirGLCommand::BIND_OBJECT));
    m_buffer.append(handle.value()); // VIRGL_OBJ_BIND_HANDLE
}

void CommandBufferBuilder::append_create_vertex_elements(ObjectHandle handle) {
    constexpr size_t cmd_len = 9;
    m_buffer.append(encode_command(cmd_len, AK::to_underlying(Protocol::ObjectType::VERTEX_ELEMENTS), Protocol::VirGLCommand::CREATE_OBJECT));
    m_buffer.append(handle.value());
    m_buffer.append(12); // src_offset_0
    m_buffer.append(0);  // instance_divisor_0
    m_buffer.append(0);  // vertex_buffer_index_0
    m_buffer.append(29); // src_format_0 (PIPE_FORMAT_R32G32_FLOAT = 29)
    m_buffer.append(0); // src_offset_1
    m_buffer.append(0);  // instance_divisor_1
    m_buffer.append(0);  // vertex_buffer_index_1
    m_buffer.append(30); // src_format_1 (PIPE_FORMAT_R32G32B32_FLOAT = 30)
}

void CommandBufferBuilder::append_bind_vertex_elements(ObjectHandle handle) {
    m_buffer.append(encode_command(1, AK::to_underlying(Protocol::ObjectType::VERTEX_ELEMENTS), Protocol::VirGLCommand::BIND_OBJECT));
    m_buffer.append(handle.value()); // VIRGL_OBJ_BIND_HANDLE
}

void CommandBufferBuilder::append_create_surface(ResourceID drawtarget_resource, ObjectHandle drawtarget_handle) {
    m_buffer.append(encode_command(5, to_underlying(Protocol::ObjectType::SURFACE), Protocol::VirGLCommand::CREATE_OBJECT));
    m_buffer.append(drawtarget_handle.value());
    m_buffer.append(drawtarget_resource.value());
    m_buffer.append(to_underlying(Protocol::TextureFormat::VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM));
    m_buffer.append(0);
    m_buffer.append(0);
}

void CommandBufferBuilder::append_set_framebuffer_state(ObjectHandle handle) {
    m_buffer.append(encode_command(3, 0, Protocol::VirGLCommand::SET_FRAMEBUFFER_STATE));
    m_buffer.append(1); // nr_cbufs
    m_buffer.append(0); // zsurf_handle
    m_buffer.append(handle.value()); // surf_handle
}

void CommandBufferBuilder::append_gl_viewport() {
    constexpr float width = 1024;
    constexpr float height = 768;
    m_buffer.append(encode_command(7, 0, Protocol::VirGLCommand::SET_VIEWPORT_STATE));
    m_buffer.append(0);
    m_buffer.append(bit_cast<u32>(width/2));  // scale_x
    m_buffer.append(bit_cast<u32>(height/2)); // scale_y
    m_buffer.append(bit_cast<u32>(0.5f));     // scale_z
    m_buffer.append(bit_cast<u32>(width/2));  // translate_x
    m_buffer.append(bit_cast<u32>(height/2)); // translate_y
    m_buffer.append(bit_cast<u32>(0.5f));     // translate_z
}

void CommandBufferBuilder::append_set_framebuffer_state_no_attach() {
    constexpr u32 width = 1024;
    constexpr u32 height = 768;
    m_buffer.append(encode_command(2, 0, Protocol::VirGLCommand::SET_FRAMEBUFFER_STATE_NO_ATTACH));
    m_buffer.append((height << 16) | width); // (height << 16 | width)
    m_buffer.append(0);                      // (samples << 16 | layers)
}

void CommandBufferBuilder::append_set_constant_buffer(Vector<float> const& constant_buffer) {
    m_buffer.append(encode_command(constant_buffer.size() + 2, 0, Protocol::VirGLCommand::SET_CONSTANT_BUFFER));
    m_buffer.append(AK::to_underlying(Gallium::ShaderType::SHADER_VERTEX)); // shader_type
    m_buffer.append(0); // index (currently unused according to virglrenderer source code)
    for (auto v: constant_buffer) {
        m_buffer.append(AK::bit_cast<u32>(v));
    }
}

void CommandBufferBuilder::append_create_shader(ObjectHandle handle, Gallium::ShaderType shader_type, const char* shader_data) {
    size_t shader_len = strlen(shader_data) + 1; // Need to remember to copy null terminator as well if needed
    size_t params_length_in_words = 5 + ((shader_len+3)/4);
    auto used = m_buffer.size();
    m_buffer.resize(used + params_length_in_words + 1);
    m_buffer[used + 0] = encode_command(params_length_in_words, to_underlying(Protocol::ObjectType::SHADER), Protocol::VirGLCommand::CREATE_OBJECT);
    m_buffer[used + 1] = handle.value(); // VIRGL_OBJ_CREATE_HANDLE
    m_buffer[used + 2] = to_underlying(shader_type);
    m_buffer[used + 3] = 0; // VIRGL_OBJ_SHADER_OFFSET
    m_buffer[used + 4] = shader_len;
    m_buffer[used + 5] = 0; // VIRGL_OBJ_SHADER_NUM_TOKENS
    memcpy(&m_buffer[used + 6], shader_data, shader_len);
}

void CommandBufferBuilder::append_bind_shader(ObjectHandle handle, Gallium::ShaderType shader_type) {
    m_buffer.append(encode_command(2, 0, Protocol::VirGLCommand::BIND_SHADER));
    m_buffer.append(handle.value()); // VIRGL_OBJ_BIND_HANDLE
    m_buffer.append(to_underlying(shader_type));
}
