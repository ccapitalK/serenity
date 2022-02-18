/*
* Copyright (c) 2022, Sahan Fernando <sahan.h.fernando@gmail.com>
*
* SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/ioctl_numbers.h>

#include "CommandBufferBuilder.h"
#include "VirGLProtocol.h"
#include "Widget.h"

static u32 encode_command(u32 length, u32 mid, Protocol::VirGLCommand command) {
    u32 command_value = to_underlying(command);
    return (length << 16) | ((mid & 0xff) << 8) | (command_value & 0xff);
};

class CommandBuilder {
public:
    CommandBuilder() = delete;
    CommandBuilder(Vector<u32> &buffer, Protocol::VirGLCommand command, u32 mid)
        : m_buffer(buffer)
        , m_start_offset(buffer.size())
        , m_command(command)
        , m_command_mid(mid) {
        m_buffer.append(0);
    }
    void appendu32(u32 value) {
        VERIFY(!m_finalized);
        m_buffer.append(value);
    }
    void appendf32(float value) {
        VERIFY(!m_finalized);
        m_buffer.append(bit_cast<u32>(value));
    }
    void appendf64(double value) {
        VERIFY(!m_finalized);
        m_buffer.append(0);
        m_buffer.append(0);
        auto *depth = (u64*)(&m_buffer[m_buffer.size() - 2]);
        *depth = AK::bit_cast<u64>(value);
    }
    void append_string_null_padded(StringView string) {
        // Remember to have at least one null terminator byte
        auto length = string.length() + 1;
        auto num_required_words = (length+sizeof(u32)-1)/sizeof(u32);
        m_buffer.resize(m_buffer.size() + num_required_words);
        char *dest = (char*)m_buffer[m_buffer.size() - num_required_words];
        memcpy(dest, string.characters_without_null_termination(), string.length());
        // Pad end with null bytes
        for (size_t i = 0; i < 4 * num_required_words; ++i) {
            dest[i] = 0;
        }
    }
    void finalize() {
        if (!m_finalized) {
            m_finalized = true;
            size_t num_elems = m_buffer.size() - m_start_offset - 1;
            m_buffer[m_start_offset] = encode_command(num_elems, m_command_mid, m_command);
        }
    }
    ~CommandBuilder() {
        if (!m_finalized)
            finalize();
    }

private:
    Vector<u32> &m_buffer;
    size_t m_start_offset;
    Protocol::VirGLCommand m_command;
    u32 m_command_mid;
    bool m_finalized {false};
};

void CommandBufferBuilder::append_transfer3d(ResourceID resource, size_t width, size_t height, size_t depth, size_t direction) {
    CommandBuilder builder(m_buffer, Protocol::VirGLCommand::TRANSFER3D, 0);
    builder.appendu32(resource.value()); // res_handle
    builder.appendu32(0); // level
    builder.appendu32(242); // usage
    builder.appendu32(0); // stride
    builder.appendu32(0); // layer_stride
    builder.appendu32(0); // x
    builder.appendu32(0); // y
    builder.appendu32(0); // z
    builder.appendu32(width); // width
    builder.appendu32(height); // height
    builder.appendu32(depth); // depth
    builder.appendu32(0); // data_offset
    builder.appendu32(direction); // direction
}

void CommandBufferBuilder::append_transfer3d_flat(ResourceID resource, size_t length) {
    CommandBuilder builder(m_buffer, Protocol::VirGLCommand::TRANSFER3D, 0);
    builder.appendu32(resource.value()); // res_handle
    builder.appendu32(0); // level
    builder.appendu32(242); // usage
    builder.appendu32(0); // stride
    builder.appendu32(0); // layer_stride
    builder.appendu32(0); // x
    builder.appendu32(0); // y
    builder.appendu32(0); // z
    builder.appendu32(length); // width
    builder.appendu32(1); // height
    builder.appendu32(1); // depth
    builder.appendu32(0); // data_offset
    builder.appendu32(VIRGL_DATA_DIR_GUEST_TO_HOST); // direction
}

void CommandBufferBuilder::append_end_transfers_3d() {
    CommandBuilder builder(m_buffer, Protocol::VirGLCommand::END_TRANSFERS, 0);
}

void CommandBufferBuilder::append_draw_vbo(u32 count) {
    CommandBuilder builder(m_buffer, Protocol::VirGLCommand::DRAW_VBO, 0);
    builder.appendu32(0); // start
    builder.appendu32(count); // count
    builder.appendu32(AK::to_underlying(Protocol::PipePrimitiveTypes::TRIANGLES)); // mode
    builder.appendu32(0); // indexed
    builder.appendu32(1); // instance_count
    builder.appendu32(0); // index_bias
    builder.appendu32(0); // start_instance
    builder.appendu32(0); // primitive_restart
    builder.appendu32(0); // restart_index
    builder.appendu32(0); // min_index
    builder.appendu32(0xffffffff); // max_index
    builder.appendu32(0); // cso
}

void CommandBufferBuilder::append_gl_clear(float r, float g, float b) {
    CommandBuilder builder(m_buffer, Protocol::VirGLCommand::CLEAR, 0);
    builder.appendu32(4);
    builder.appendf32(r);
    builder.appendf32(g);
    builder.appendf32(b);
    builder.appendf32(1.0f);
    builder.appendf64(1.0);
    builder.appendu32(0);
}

void CommandBufferBuilder::append_set_vertex_buffers(u32 stride, u32 offset, ResourceID resource) {
    CommandBuilder builder(m_buffer, Protocol::VirGLCommand::SET_VERTEX_BUFFERS, 0);
    builder.appendu32(stride);
    builder.appendu32(offset);
    builder.appendu32(resource.value());
}

void CommandBufferBuilder::append_create_blend(ObjectHandle handle) {
    CommandBuilder builder(m_buffer, Protocol::VirGLCommand::CREATE_OBJECT, to_underlying(Protocol::ObjectType::BLEND));
    builder.appendu32(handle.value());
    builder.appendu32(4); // Enable dither flag, and nothing else
    builder.appendu32(0);
    builder.appendu32(0x78000000); // Enable all bits of color mask for color buffer 0, and nothing else
    for (size_t i = 1; i < 8; ++i) {
        builder.appendu32(0); // Explicitly disable all flags for other color buffers
    }
}

void CommandBufferBuilder::append_bind_blend(ObjectHandle handle) {
    CommandBuilder builder(m_buffer, Protocol::VirGLCommand::BIND_OBJECT, to_underlying(Protocol::ObjectType::BLEND));
    builder.appendu32(handle.value()); // VIRGL_OBJ_BIND_HANDLE
}

void CommandBufferBuilder::append_create_vertex_elements(ObjectHandle handle) {
    CommandBuilder builder(m_buffer, Protocol::VirGLCommand::CREATE_OBJECT, to_underlying(Protocol::ObjectType::VERTEX_ELEMENTS));
    builder.appendu32(handle.value());
    builder.appendu32(12); // src_offset_0
    builder.appendu32(0);  // instance_divisor_0
    builder.appendu32(0);  // vertex_buffer_index_0
    builder.appendu32(29); // src_format_0 (PIPE_FORMAT_R32G32_FLOAT = 29)
    builder.appendu32(0); // src_offset_1
    builder.appendu32(0);  // instance_divisor_1
    builder.appendu32(0);  // vertex_buffer_index_1
    builder.appendu32(30); // src_format_1 (PIPE_FORMAT_R32G32B32_FLOAT = 30)
}

void CommandBufferBuilder::append_bind_vertex_elements(ObjectHandle handle) {
    CommandBuilder builder(m_buffer, Protocol::VirGLCommand::BIND_OBJECT, to_underlying(Protocol::ObjectType::VERTEX_ELEMENTS));
    builder.appendu32(handle.value()); // VIRGL_OBJ_BIND_HANDLE
}

void CommandBufferBuilder::append_create_surface(ResourceID drawtarget_resource, ObjectHandle drawtarget_handle) {
    CommandBuilder builder(m_buffer, Protocol::VirGLCommand::CREATE_OBJECT, to_underlying(Protocol::ObjectType::SURFACE));
    builder.appendu32(drawtarget_handle.value());
    builder.appendu32(drawtarget_resource.value());
    builder.appendu32(to_underlying(Protocol::TextureFormat::VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM));
    builder.appendu32(0);
    builder.appendu32(0);
}

void CommandBufferBuilder::append_set_framebuffer_state(ObjectHandle handle) {
    CommandBuilder builder(m_buffer, Protocol::VirGLCommand::SET_FRAMEBUFFER_STATE, 0);
    builder.appendu32(1); // nr_cbufs
    builder.appendu32(0); // zsurf_handle
    builder.appendu32(handle.value()); // surf_handle
}

void CommandBufferBuilder::append_gl_viewport() {
    CommandBuilder builder(m_buffer, Protocol::VirGLCommand::SET_VIEWPORT_STATE, 0);
    builder.appendu32(0);
    builder.appendf32(DRAWTARGET_WIDTH/2);  // scale_x
    builder.appendf32((DRAWTARGET_HEIGHT/2)); // scale_y (flipped, due to VirGL being different from our coordinate space)
    builder.appendf32(0.5f);                // scale_z
    builder.appendf32(DRAWTARGET_WIDTH/2);  // translate_x
    builder.appendf32(DRAWTARGET_HEIGHT/2); // translate_y
    builder.appendf32(0.5f);                // translate_z
}

void CommandBufferBuilder::append_set_framebuffer_state_no_attach() {
    m_buffer.append(encode_command(2, 0, Protocol::VirGLCommand::SET_FRAMEBUFFER_STATE_NO_ATTACH));
    m_buffer.append((DRAWTARGET_HEIGHT << 16) | DRAWTARGET_WIDTH); // (height << 16 | width)
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
