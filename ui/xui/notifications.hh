//
// xemu User Interface
//
// Copyright (C) 2020-2022 Matt Borgerson
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#pragma once
#include <stdint.h>
#include <deque>
#include <string>

#include "../xemu-notifications.h"

class NotificationManager
{
private:
    /* Status notifications are newest-wins, so a queue just creates churn. */
    std::string m_pending_msg;
    std::deque<std::string> m_error_queue;

    const int kNotificationDuration = 1500;
    uint32_t m_notification_end_time = 0;
    std::string m_msg;
    bool m_pending = false;
    bool m_active = false;

public:
    NotificationManager();
    void QueueNotification(const char *msg);
    void QueueError(const char *msg);
    void Draw();

private:
    void DrawNotification(float t, const char *msg);
};

extern NotificationManager notification_manager;
