/*
 * Copyright (c) 2022, Sahan Fernando <sahan.h.fernando@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/String.h>
#include <AK/Vector.h>
#include <LibMain/Main.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/ioctl_numbers.h>
#include <unistd.h>

#include "CommandBufferBuilder.h"
#include "VirGLProtocol.h"

[[maybe_unused]] static const char* frag_shader =
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

int gpu_fd;
ResourceID vbo_resource_id;
ResourceID drawtarget;
ObjectHandle blend_handle;
ObjectHandle drawtarget_surface_handle;
ObjectHandle ve_handle;
ObjectHandle frag_shader_handle;
ObjectHandle vert_shader_handle;

static ObjectHandle allocate_handle() {
    static u32 last_allocated_handle = 32;
    return {++last_allocated_handle};
}

static void upload_command_buffer(Vector<u32> const& command_buffer) {
    VirGLCommandBuffer command_buffer_descriptor {
        .data = command_buffer.data(),
        .num_elems = command_buffer.size(),
    };
    VERIFY(ioctl(gpu_fd, VIRGL_IOCTL_SUBMIT_CMD, &command_buffer_descriptor) >= 0);
}

static void init() {
    // Open the device
    gpu_fd = open("/dev/gpu0", O_RDWR);
    VERIFY(gpu_fd >= 0);
    // Do kernel space setup (disable writes from display server)
    VERIFY(ioctl(gpu_fd, VIRGL_IOCTL_SETUP_DEMO) >= 0);
    // Get info
    VirGLDisplayInfo display_info;
    VERIFY(ioctl(gpu_fd, VIRGL_IOCTL_GET_DISPLAY_INFO, &display_info) >= 0);
    drawtarget = {display_info.drawtarget_id};
    // Create a VertexElements resource
    VirGL3DResourceSpec vbo_spec {
        .target = AK::to_underlying(Gallium::PipeTextureTarget::BUFFER), // pipe_texture_target
        .format = 45, // pipe_to_virgl_format
        .bind = VIRGL_BIND_VERTEX_BUFFER,
        .width = PAGE_SIZE,
        .height = 1,
        .depth = 1,
        .array_size = 1,
        .last_level = 0,
        .nr_samples = 0,
        .flags = 0,
        .created_resource_id = 0,
    };
    VERIFY(ioctl(gpu_fd, VIRGL_IOCTL_CREATE_RESOURCE, &vbo_spec) >= 0);
    vbo_resource_id = vbo_spec.created_resource_id;

    // Initialize all required state
    CommandBufferBuilder builder;
    // Create and set the blend, to control the color mask
    blend_handle = allocate_handle();
    builder.append_create_blend(blend_handle);
    builder.append_bind_blend(blend_handle);
    // Create Drawtarget surface
    drawtarget_surface_handle = allocate_handle();
    builder.append_create_surface(drawtarget, drawtarget_surface_handle);
    builder.append_set_framebuffer_state(drawtarget_surface_handle);
    builder.append_set_framebuffer_state_no_attach();
    // Set the vertex buffer
    builder.append_set_vertex_buffers(20, 0, vbo_resource_id);
    // Create and bind the shaders
    frag_shader_handle = allocate_handle();
    vert_shader_handle = allocate_handle();
    // Create Fragment shader
    builder.append_create_shader(frag_shader_handle, Gallium::ShaderType::SHADER_FRAGMENT, frag_shader);
    builder.append_bind_shader(frag_shader_handle, Gallium::ShaderType::SHADER_FRAGMENT);
    // Create Vertex shader
    builder.append_create_shader(vert_shader_handle, Gallium::ShaderType::SHADER_VERTEX, vert_shader);
    builder.append_bind_shader(vert_shader_handle, Gallium::ShaderType::SHADER_VERTEX);
    // Create a VertexElements object
    ve_handle = allocate_handle();
    builder.append_create_vertex_elements(ve_handle);
    builder.append_bind_vertex_elements(ve_handle);
    // Set the Viewport
    builder.append_gl_viewport();
    // Set the constant buffer
    builder.append_set_constant_buffer({
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0,
    });
    // Upload buffer
    upload_command_buffer(builder.build());
}

struct VertexData {
    float r;
    float g;
    float b;
    float x;
    float y;
};

static VertexData gen_rand_colored_vertex_at(float x, float y) {
    return {
        .r = ((float)(rand() % 256)) / 255.f,
        .g = ((float)(rand() % 256)) / 255.f,
        .b = ((float)(rand() % 256)) / 255.f,
        .x = x,
        .y = y,
    };
}

static void draw_frame() {
    float top_x_ordinate = 0.9 - ((rand() % 18) / 10.0);
    VertexData vertices[3] = {
        gen_rand_colored_vertex_at(-0.8, -0.8),
        gen_rand_colored_vertex_at(0.8, -0.8),
        gen_rand_colored_vertex_at(top_x_ordinate, 0.9),
    };
    VirGLTransferDescriptor descriptor {
        .data = (void*)vertices,
        .offset_in_region = 0,
        .num_bytes = sizeof(vertices),
        .direction = VIRGL_DATA_DIR_GUEST_TO_HOST,
    };
    dbgln("Going to transfer vertex data");
    VERIFY(ioctl(gpu_fd, VIRGL_IOCTL_TRANSFER_DATA, &descriptor) >= 0);
    dbgln("Going to draw");
    // Create command buffer
    CommandBufferBuilder builder;
    // Transfer data to vbo
    builder.append_transfer3d_flat(vbo_resource_id, sizeof(vertices));
    builder.append_end_transfers_3d();
    // Clear the framebuffer
    builder.append_gl_clear(0, 0, 0.5);
    // Draw the vbo
    builder.append_draw_vbo(3);
    upload_command_buffer(builder.build());
    VERIFY(ioctl(gpu_fd, VIRGL_IOCTL_FLUSH_DISPLAY) >= 0);
}

static void finish() {
    VERIFY(ioctl(gpu_fd, VIRGL_IOCTL_FINISH_DEMO) >= 0);
}

ErrorOr<int> serenity_main(Main::Arguments)
{
    init();
    for (int i = 0; i < 10; ++i) {
        draw_frame();
        usleep(200000);
    }
    finish();
    return {0};
}
