# Headless: blender -b -P tools/make_models.py -- <preview dir> source
# Builds the gamepad, keyboard and joystick as one vertex-coloured, low-poly mesh named "controller" each, saves <name>.blend into
# source/ (overwriting edits made there) and renders a preview PNG of each into the preview dir.
import bpy, bmesh, sys, os, math
from mathutils import Vector, Matrix

out = sys.argv[sys.argv.index("--") + 1]
blend_out = sys.argv[sys.argv.index("--") + 2]
os.makedirs(out, exist_ok=True)
os.makedirs(blend_out, exist_ok=True)

GREY = (0.62, 0.62, 0.66, 1)
DARK = (0.10, 0.10, 0.11, 1)
MID = (0.30, 0.30, 0.33, 1)
GREEN = (0.10, 0.55, 0.15, 1)
RED = (0.75, 0.10, 0.10, 1)
BLUE = (0.10, 0.20, 0.70, 1)
YELLOW = (0.85, 0.65, 0.05, 1)
KEY = (0.05, 0.05, 0.06, 1)
KEYTOP = (0.62, 0.62, 0.66, 1)
BLUE_KEY = (0.35, 0.55, 0.85, 1)


def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)


class Builder:
    def __init__(self):
        self.bm = bmesh.new()
        self.col = self.bm.loops.layers.color.new("Col")

    def paint(self, faces, color):
        for f in faces:
            for l in f.loops:
                l[self.col] = color

    def box(self, center, size, color, top_color=None):
        r = bmesh.ops.create_cube(self.bm, size=1.0)
        verts = r["verts"]
        bmesh.ops.scale(self.bm, vec=Vector(size), verts=verts)
        bmesh.ops.translate(self.bm, vec=Vector(center), verts=verts)
        faces = list({f for v in verts for f in v.link_faces})
        self.paint(faces, color)
        if top_color:
            top = max(faces, key=lambda f: f.calc_center_median().z)
            self.paint([top], top_color)
        return faces

    def cyl(self, center, radius, depth, color, segs=8, top_color=None):
        r = bmesh.ops.create_cone(self.bm, cap_ends=True, segments=segs, radius1=radius, radius2=radius, depth=depth)
        verts = r["verts"]
        bmesh.ops.translate(self.bm, vec=Vector(center), verts=verts)
        faces = list({f for v in verts for f in v.link_faces})
        self.paint(faces, color)
        if top_color:
            top = max(faces, key=lambda f: f.calc_center_median().z)
            self.paint([top], top_color)
        return faces

    def finish(self, name):
        me = bpy.data.meshes.new(name)
        self.bm.to_mesh(me)
        self.bm.free()
        me.color_attributes.active_color = me.color_attributes["Col"]
        me.color_attributes.render_color_index = me.color_attributes.active_color_index
        ob = bpy.data.objects.new(name, me)
        bpy.context.scene.collection.objects.link(ob)
        mat = bpy.data.materials.new("vcol")
        mat.use_nodes = True
        nt = mat.node_tree
        bsdf = next(n for n in nt.nodes if n.type == "BSDF_PRINCIPLED")
        attr = nt.nodes.new("ShaderNodeVertexColor")
        attr.layer_name = "Col"
        nt.links.new(attr.outputs[0], bsdf.inputs[0])
        me.materials.append(mat)
        return ob


# Right half of a two-stick pad's outline (x >= 0, +y = the far edge), clockwise from the top centre; mirrored for the left.
PAD_HALF = [(0, 1.05), (0.7, 1.05), (1.25, 1.0), (1.6, 0.85), (1.85, 0.55), (1.98, 0.15), (2.02, -0.3), (1.98, -0.8),
            (1.85, -1.25), (1.62, -1.55), (1.35, -1.62), (1.1, -1.5), (0.9, -1.2), (0.7, -0.85), (0.45, -0.7), (0, -0.68)]

SIDE = (0.42, 0.42, 0.46, 1)
LIGHT = (0.78, 0.78, 0.82, 1)


def slab(b, outline, z0, z1, side, top, inset=0.0, rise=0.0, inset_color=None):
    """An outline extruded from z0 to z1; with an inset, its top is a raised panel (a soft bevel) in inset_color."""
    verts = [b.bm.verts.new((x, y, z0)) for (x, y) in outline]
    base = b.bm.faces.new(list(reversed(verts)))
    r = bmesh.ops.extrude_face_region(b.bm, geom=[base])
    up = [e for e in r["geom"] if isinstance(e, bmesh.types.BMVert)]
    bmesh.ops.translate(b.bm, vec=Vector((0, 0, z1 - z0)), verts=up)
    top_face = next(f for f in r["geom"] if isinstance(f, bmesh.types.BMFace))
    b.paint(list({f for v in up for f in v.link_faces}) + [base], side)
    b.paint([top_face], top)

    if inset:
        res = bmesh.ops.inset_region(b.bm, faces=[top_face], thickness=inset, depth=rise)
        b.paint(res["faces"], inset_color or top)


def ring(cx, cy, r, segs):
    return [(cx + r * math.cos(2 * math.pi * i / segs), cy + r * math.sin(2 * math.pi * i / segs)) for i in range(segs)][::-1]


def gamepad():
    b = Builder()
    outline = PAD_HALF + [(-x, y) for (x, y) in reversed(PAD_HALF[1:-1])]

    # Body: one moulded shell with the grips flowing out of it, a raised top panel for the bevel.
    slab(b, outline, 0.0, 0.42, SIDE, GREY, inset=0.12, rise=0.08, inset_color=GREY)

    # Bumpers along the far edge, triggers behind them.
    for s in (-1, 1):
        yaw(b, rbox(b, (1.2 * s, 1.0, 0.03), (0.6, 0.32, 0.12), MID, 0.1), -14 * s, (1.2 * s, 1.0, 0))
        yaw(b, rbox(b, (1.2 * s, 1.07, 0.22), (1.05, 0.2, 0.18), DARK, 0.08), -14 * s, (1.2 * s, 1.07, 0))

    # Sticks: a dark well and a cap with a lighter top. Left upper, right lower (the offset layout).
    for (x, y) in ((-1.15, 0.3), (0.62, -0.45)):
        b.cyl((x, y, 0.53), 0.36, 0.06, DARK, segs=14)
        b.cyl((x, y, 0.63), 0.26, 0.16, MID, segs=12, top_color=SIDE)

    # D-pad: a cross on a round base.
    b.cyl((-0.62, -0.45, 0.53), 0.34, 0.06, DARK, segs=12)
    b.box((-0.62, -0.45, 0.6), (0.52, 0.17, 0.1), MID)
    b.box((-0.62, -0.45, 0.6), (0.17, 0.52, 0.1), MID)

    # Face buttons on a ring: A bottom, B right, X left, Y top.
    fx, fy = 1.2, 0.3
    b.cyl((fx, fy, 0.53), 0.5, 0.04, SIDE, segs=16)

    for (dx, dy, c) in ((0, -0.28, GREEN), (0.28, 0, RED), (-0.28, 0, BLUE), (0, 0.28, YELLOW)):
        b.cyl((fx + dx, fy + dy, 0.6), 0.13, 0.12, c, segs=10)

    # View / menu, and a round home button between them.
    b.box((-0.32, 0.38, 0.55), (0.18, 0.1, 0.06), DARK)
    b.box((0.32, 0.38, 0.55), (0.18, 0.1, 0.06), DARK)
    b.cyl((0, 0.68, 0.55), 0.15, 0.06, LIGHT, segs=12)
    return b.finish("controller")


def keyboard():
    b = Builder()
    rows = [
        ["ESC", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "=", ("BS", 2.0)],
        [("TAB", 1.5), "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "[", "]", ("\\", 1.5)],
        [("CAPS", 1.75), "A", "S", "D", "F", "G", "H", "J", "K", "L", ";", "'", ("ENT", 2.25)],
        [("SH", 2.25), "Z", "X", "C", "V", "B", "N", "M", ",", ".", "/", ("SH", 2.75)],
        [("C", 1.25), ("W", 1.25), ("A", 1.25), ("SP", 6.25), ("A", 1.25), ("W", 1.25), ("M", 1.25), ("C", 1.25)],
    ]
    u = 0.3
    width = 15 * u
    b.box((0, 0, -0.08), (width + 0.25, 5 * u + 0.25, 0.16), DARK)
    highlight = {"W": GREEN, "A": BLUE_KEY, "S": BLUE_KEY, "D": BLUE_KEY}
    for r, row in enumerate(rows):
        x = -width / 2
        y = (2 - r) * u
        for k in row:
            name, w = (k, 1.0) if isinstance(k, str) else k
            cx = x + w * u / 2
            top = highlight.get(name, KEYTOP) if r in (1, 2) and len(name) == 1 else KEYTOP
            b.box((cx, y, 0.175), (w * u - 0.07, u - 0.07, 0.35), KEY, top_color=top)
            x += w * u
    return b.finish("controller")


BASE_TOP = (0.82, 0.82, 0.84, 1)
BASE_PANEL = (0.72, 0.72, 0.75, 1)
BASE_SIDE = (0.55, 0.55, 0.58, 1)
STICK = (0.09, 0.09, 0.10, 1)


def yaw(b, faces, degrees, pivot):
    """Rotates a part about the vertical axis (to follow a curved edge)."""
    verts = list({v for f in faces for v in f.verts})
    bmesh.ops.rotate(b.bm, cent=Vector(pivot), matrix=Matrix.Rotation(math.radians(degrees), 3, "Z"), verts=verts)


def tilt(b, faces, degrees, pivot):
    """Rotates a part about the x axis (positive leans its top toward the far edge)."""
    verts = list({v for f in faces for v in f.verts})
    bmesh.ops.rotate(b.bm, cent=Vector(pivot), matrix=Matrix.Rotation(math.radians(-degrees), 3, "X"), verts=verts)


def rbox(b, center, size, color, radius, segs=2):
    """A box with rounded (bevelled) edges."""
    faces = b.box(center, size, color)
    verts = list({v for f in faces for v in f.verts})
    edges = list({e for v in verts for e in v.link_edges})
    r = bmesh.ops.bevel(b.bm, geom=verts + edges, offset=radius, segments=segs, profile=0.5, affect="EDGES")
    out = list({f for v in verts + r["verts"] if v.is_valid for f in v.link_faces})
    b.paint(out, color)
    return out


def rounded_rect(w, d, r, segs=4):
    """Outline of a w x d rectangle with rounded corners, clockwise from above."""
    pts = []

    for cx, cy, start in ((w / 2 - r, d / 2 - r, 0), (-w / 2 + r, d / 2 - r, 90), (-w / 2 + r, -d / 2 + r, 180), (w / 2 - r, -d / 2 + r, 270)):
        for i in range(segs + 1):
            a = math.radians(start + 90 * i / segs)
            pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))

    return pts[::-1]


def joystick():
    # A desktop flight stick after the Rogue Squadron 3D reference card's: wide light base with a throttle slider,
    # a black pistol grip in a rubber boot, its head angled forward with the hat and two thumb buttons on top.
    b = Builder()
    slab(b, rounded_rect(3.2, 2.4, 0.45), 0.0, 0.45, BASE_SIDE, BASE_TOP, inset=0.14, rise=0.04, inset_color=BASE_PANEL)

    # THRUST/BRAKE slider on the left, a small panel slot on the right.
    b.box((-1.0, -0.25, 0.5), (0.22, 1.3, 0.04), STICK)
    rbox(b, (-1.0, 0.05, 0.6), (0.4, 0.28, 0.18), MID, 0.06)
    b.box((1.0, -0.4, 0.5), (0.55, 0.18, 0.04), STICK)

    # A rubber boot tapering into an oval grip that narrows toward the head, and the trigger on its front.
    boot = b.bm
    r = bmesh.ops.create_cone(boot, cap_ends=True, segments=14, radius1=0.6, radius2=0.3, depth=0.5)
    bmesh.ops.translate(boot, vec=Vector((0, 0.05, 0.7)), verts=r["verts"])
    b.paint(list({f for v in r["verts"] for f in v.link_faces}), STICK)
    r = bmesh.ops.create_cone(boot, cap_ends=True, segments=14, radius1=0.3, radius2=0.26, depth=1.3)
    bmesh.ops.scale(boot, vec=Vector((0.9, 1.1, 1)), verts=r["verts"])
    bmesh.ops.translate(boot, vec=Vector((0, 0.05, 1.55)), verts=r["verts"])
    b.paint(list({f for v in r["verts"] for f in v.link_faces}), STICK)
    rbox(b, (0, 0.36, 1.95), (0.18, 0.2, 0.34), MID, 0.06)

    # Head, leaning forward over the grip, with the hat and thumb buttons on its top.
    pivot = (0, 0.05, 2.15)
    tilt(b, rbox(b, (0, 0.6, 2.3), (0.62, 1.35, 0.42), STICK, 0.16, segs=3), 24, pivot)
    tilt(b, b.cyl((0, 0.75, 2.55), 0.15, 0.1, MID, segs=12, top_color=GREY), 24, pivot)

    for y in (0.3, 1.1):
        tilt(b, b.cyl((0.18, y, 2.53), 0.08, 0.08, GREY, segs=10), 24, pivot)

    return b.finish("controller")


def preview(ob, path):
    scn = bpy.context.scene
    scn.render.engine = "BLENDER_WORKBENCH"
    scn.display.shading.light = "STUDIO"
    scn.display.shading.color_type = "VERTEX"
    scn.render.resolution_x = 640
    scn.render.resolution_y = 400
    scn.render.film_transparent = False
    # Three-quarter view framed on the model's bounds.
    lo = Vector([min(v.co[a] for v in ob.data.vertices) for a in range(3)])
    hi = Vector([max(v.co[a] for v in ob.data.vertices) for a in range(3)])
    centre, d = (lo + hi) / 2, max(hi - lo) * 1.6 + 1.0
    cam_data = bpy.data.cameras.new("cam")
    cam = bpy.data.objects.new("cam", cam_data)
    scn.collection.objects.link(cam)
    cam.location = centre + Vector((d * 0.45, -d, d * 0.75))
    cam.rotation_euler = (centre - cam.location).to_track_quat("-Z", "Y").to_euler()
    cam_data.lens = 40
    scn.camera = cam
    scn.render.filepath = path
    bpy.ops.render.render(write_still=True)
    bpy.data.objects.remove(cam)


for name, fn in (("gamepad", gamepad), ("keyboard", keyboard), ("joystick", joystick)):
    reset()
    ob = fn()
    me = ob.data
    print(f"{name}: {len(me.vertices)} verts, {len(me.polygons)} faces")
    preview(ob, os.path.join(out, f"{name}_preview.png"))
    bpy.context.preferences.filepaths.save_version = 0
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(blend_out, f"{name}.blend"))
