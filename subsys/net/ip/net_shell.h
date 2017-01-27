/** @file
 @brief Network shell handler

 This is not to be included by the application.
 */

/*
 * Copyright (c) 2016 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __NET_SHELL_H
#define __NET_SHELL_H

#if defined(CONFIG_NET_SHELL)
void net_shell_init(void);
#else
#define net_shell_init(...)
#endif /* CONFIG_NET_SHELL */

#endif /* __NET_SHELL_H */
