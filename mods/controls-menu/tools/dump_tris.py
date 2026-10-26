# blender -b <file.blend> -P dump_tris.py -- <out.json>: the "controller" mesh's faces (quads kept, larger faces triangulated),
# Blender coords, corner colours 0..255. The game's own models are quads.
import bpy, bmesh, json, sys
out = sys.argv[sys.argv.index("--") + 1]
ob = bpy.data.objects["controller"]
bm = bmesh.new()
bm.from_mesh(ob.data)
bmesh.ops.triangulate(bm, faces=[f for f in bm.faces if len(f.verts) > 4])
col = bm.loops.layers.color["Col"]
faces = []
for f in bm.faces:
    faces.append({"p": [list(l.vert.co) for l in f.loops], "c": [[round(c * 255) for c in l[col]] for l in f.loops]})
json.dump(faces, open(out, "w"))
print("faces", len(faces))
