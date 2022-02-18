/*
* Copyright (c) 2022, Sahan Fernando <sahan.h.fernando@gmail.com>
*
* SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGUI/Label.h>
#include <LibGUI/Painter.h>

#include "Widget.h"

Demo::Demo()
{
    m_bitmap = Gfx::Bitmap::try_create(Gfx::BitmapFormat::BGRx8888, { DRAWTARGET_WIDTH, DRAWTARGET_HEIGHT }).release_value_but_fixme_should_propagate_errors();
    m_bitmap->fill(Gfx::Color::Black);

    m_accumulated_time = 0;
    m_cycles = 0;
    m_phase = 0;

    stop_timer();
    start_timer(200);
}

Demo::~Demo() {}

void Demo::paint_event(GUI::PaintEvent& event)
{
    GUI::Painter painter(*this);
    painter.add_clip_rect(event.rect());
    painter.draw_scaled_bitmap(rect(), *m_bitmap, m_bitmap->rect());
}

void Demo::timer_event(Core::TimerEvent&)
{
    update_frame(m_bitmap);
    update();
}