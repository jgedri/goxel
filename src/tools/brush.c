/* Goxel 3D voxels editor
 *
 * copyright (c) 2017 Guillaume Chereau <guillaume@noctua-software.com>
 *
 * Goxel is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.

 * Goxel is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.

 * You should have received a copy of the GNU General Public License along with
 * goxel.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "goxel.h"

enum {
    STATE_IDLE      = 0,
    STATE_CANCEL    = 1,
    STATE_END       = 2,

    STATE_SNAPED,
    STATE_PAINT,
    STATE_WAIT_KEY_UP,

    STATE_ENTER = 0x0100,
};

typedef struct {
    mesh_t *mesh_orig; // Original mesh.
    mesh_t *mesh;      // Mesh containing only the tool path.

    bool painting;
    vec3_t start_pos;
    // Cache of the last operation.
    // XXX: could we remove this?
    struct     {
        vec3_t     pos;
        bool       pressed;
        int        mode;
    } last_op;
} data_t;

static bool check_can_skip(data_t *data, const cursor_t *curs, int mode)
{
    const bool pressed = curs->flags & CURSOR_PRESSED;
    if (    pressed == data->last_op.pressed &&
            mode == data->last_op.mode &&
            vec3_equal(curs->pos, data->last_op.pos))
        return true;
    data->last_op.pressed = pressed;
    data->last_op.mode = mode;
    data->last_op.pos = curs->pos;
    return false;
}

static box_t get_box(const vec3_t *p0, const vec3_t *p1, const vec3_t *n,
                     float r, const plane_t *plane)
{
    mat4_t rot;
    box_t box;
    if (p1 == NULL) {
        box = bbox_from_extents(*p0, r, r, r);
        box = box_swap_axis(box, 2, 0, 1);
        return box;
    }
    if (r == 0) {
        box = bbox_grow(bbox_from_points(*p0, *p1), 0.5, 0.5, 0.5);
        // Apply the plane rotation.
        rot = plane->mat;
        rot.vecs[3] = vec4(0, 0, 0, 1);
        mat4_imul(&box.mat, rot);
        return box;
    }

    // Create a box for a line:
    int i;
    const vec3_t AXES[] = {vec3(1, 0, 0), vec3(0, 1, 0), vec3(0, 0, 1)};

    box.mat = mat4_identity;
    box.p = vec3_mix(*p0, *p1, 0.5);
    box.d = vec3_sub(*p1, box.p);
    for (i = 0; i < 3; i++) {
        box.w = vec3_cross(box.d, AXES[i]);
        if (vec3_norm2(box.w) > 0) break;
    }
    if (i == 3) return box;
    box.w = vec3_mul(vec3_normalized(box.w), r);
    box.h = vec3_mul(vec3_normalized(vec3_cross(box.d, box.w)), r);
    return box;
}

static data_t *get_data(void **data_)
{
    data_t **data = (data_t**)data_;
    if (!*data) {
        *data = calloc(1, sizeof(**data));
        (*data)->mesh_orig = mesh_copy(goxel->image->active_layer->mesh);
        (*data)->mesh      = mesh_new();
    }
    return *data;
}

static int iter(int state, void **data_,
                const vec4_t *view, bool inside)
{
    data_t *data = get_data(data_);
    box_t box;
    painter_t painter2;
    mesh_t *mesh = goxel->image->active_layer->mesh;
    cursor_t *curs = &goxel->cursor;
    bool shift = curs->flags & CURSOR_SHIFT;

    curs->snap_offset = goxel->snap_offset * goxel->tool_radius +
        ((goxel->painter.mode == MODE_OVER) ? 0.5 : -0.5);

    switch (state) {
    case STATE_IDLE:
        if (curs->snaped) return STATE_SNAPED;
        break;

    case STATE_SNAPED | STATE_ENTER:
        mesh_set(data->mesh_orig, mesh);
        data->last_op.mode = 0; // Discard last op.
        break;

    case STATE_SNAPED:
        if (!curs->snaped) return STATE_CANCEL;
        if (shift)
            render_line(&goxel->rend, &data->start_pos, &curs->pos, NULL);
        if (check_can_skip(data, curs, goxel->painter.mode))
            return state;
        box = get_box(&curs->pos, NULL, &curs->normal,
                      goxel->tool_radius, NULL);

        mesh_set(mesh, data->mesh_orig);
        mesh_op(mesh, &goxel->painter, &box);
        goxel_update_meshes(goxel, MESH_LAYERS);

        if (shift) {
            render_line(&goxel->rend, &data->start_pos, &curs->pos, NULL);
            if (curs->flags & CURSOR_DOWN) {
                painter2 = goxel->painter;
                painter2.shape = &shape_cylinder;
                box = get_box(&data->start_pos, &curs->pos, &curs->normal,
                              goxel->tool_radius, NULL);
                mesh_set(mesh, data->mesh_orig);
                mesh_op(mesh, &painter2, &box);
                mesh_set(data->mesh_orig, mesh);
                goxel_update_meshes(goxel, MESH_LAYERS);
                data->start_pos = curs->pos;
            }
        }
        if (curs->flags & CURSOR_DOWN) {
            state = STATE_PAINT;
            data->last_op.mode = 0;
            data->painting = true;
            mesh_set(mesh, data->mesh_orig);
            image_history_push(goxel->image);
            mesh_clear(data->mesh);
        }
        break;

    case STATE_PAINT:
        if (!curs->snaped) return state;
        if (check_can_skip(data, curs, goxel->painter.mode))
            return state;
        if (curs->flags & CURSOR_UP) {
            data->painting = false;
            goxel->camera.target = curs->pos;
            if (shift)
                return STATE_WAIT_KEY_UP;
            mesh_set(goxel->pick_mesh, goxel->layers_mesh);
            mesh_set(data->mesh_orig, mesh);
            return STATE_IDLE;
        }
        box = get_box(&curs->pos, NULL, &curs->normal,
                      goxel->tool_radius, NULL);
        painter2 = goxel->painter;
        painter2.mode = MODE_MAX;
        mesh_op(data->mesh, &painter2, &box);
        mesh_set(mesh, data->mesh_orig);
        mesh_merge(mesh, data->mesh, goxel->painter.mode);
        goxel_update_meshes(goxel, MESH_LAYERS);
        data->start_pos = curs->pos;
        break;

    case STATE_WAIT_KEY_UP:
        if (!shift) state = STATE_IDLE;
        if (curs->snaped) state = STATE_SNAPED;
        break;
    }
    return state;
}

static int cancel(int state, void **data_)
{
    if (!(*data_)) return 0;
    data_t *data = get_data(data_);
    mesh_set(goxel->image->active_layer->mesh, data->mesh_orig);
    mesh_delete(data->mesh_orig);
    mesh_delete(data->mesh);
    free(data);
    *data_ = NULL;
    return 0;
}

static int gui(void)
{
    tool_gui_radius();
    tool_gui_smoothness();
    tool_gui_snap();
    tool_gui_mode();
    tool_gui_shape();
    tool_gui_color();
    return 0;
}

TOOL_REGISTER(TOOL_BRUSH, brush,
              .iter_fn = iter,
              .cancel_fn = cancel,
              .gui_fn = gui,
              .shortcut = "B"
)
