/*
    Yumi SDK — Umbrella Header for GUI Widgets
    Copyright (C) 2026  DevNullIsaac

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#ifndef WASM_GUI_H
#define WASM_GUI_H

/**
 * @file wasm_gui.h
 * @brief Convenience umbrella header for all GUI widget headers.
 *
 * Include this single header to get every widget type plus the
 * theme constants and widget dispatch infrastructure.
 */

#include "gui/widget.h"
#include "gui/yumi_theme.h"
#include "gui/wasm_button.h"
#include "gui/wasm_textbox.h"
#include "gui/wasm_textlabel.h"
#include "gui/wasm_scrollbar.h"
#include "gui/wasm_listview.h"
#include "gui/wasm_treeview.h"
#include "gui/wasm_picturebox.h"
#include "gui/wasm_docbox.h"
#include "gui/wasm_nodegraph.h"
#include "gui/wasm_expander.h"
#include "gui/wasm_tooltip_overlay.h"
#include "gui/wasm_toolbox.h"
#include "gui/wasm_menubar.h"
#include "gui/wasm_autolayout.h"

#endif /* WASM_GUI_H */
