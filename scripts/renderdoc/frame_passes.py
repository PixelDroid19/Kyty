# List every draw and dispatch of a RenderDoc capture with the images it writes and reads,
# and optionally save chosen images as EXR at chosen events.
# Run with: qrenderdoc --python frame_passes.py ; configuration comes from the environment:
#   RDOC_CAPTURE (the .rdc), RDOC_OUT (output directory),
#   RDOC_SAVE ("eid:resource,..." from the table, optional).
import os
import renderdoc as rd

CAPTURE = os.environ["RDOC_CAPTURE"]
OUT = os.environ["RDOC_OUT"]
SAVE = [tuple(int(v) for v in item.split(":")) for item in os.environ.get("RDOC_SAVE", "").split(",") if item]
os.makedirs(OUT, exist_ok=True)
log = open(os.path.join(OUT, "frame_passes.txt"), "w")


def say(*args):
    print(*args, file=log)
    log.flush()


def walk(actions, out):
    for action in actions:
        if action.flags & (rd.ActionFlags.Drawcall | rd.ActionFlags.Dispatch):
            out.append(action)
        walk(action.children, out)


def describe(textures, rid):
    tex = textures.get(rid)
    return "%d:%dx%d:%s" % (int(rid), tex.width, tex.height, tex.format.Name()) if tex is not None else "%d" % int(rid)


def main():
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(CAPTURE, "", None) != rd.ResultCode.Succeeded:
        say("open failed")
        return
    status, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if status != rd.ResultCode.Succeeded:
        say("replay failed", status)
        return
    textures = {t.resourceId: t for t in controller.GetTextures()}
    by_id = {int(rid): rid for rid in textures}
    actions = []
    walk(controller.GetRootActions(), actions)
    say("# eid kind indices instances W <written> R <read>")
    for action in actions:
        controller.SetFrameEvent(action.eventId, False)
        state = controller.GetPipelineState()
        stage = rd.ShaderStage.Compute if action.flags & rd.ActionFlags.Dispatch else rd.ShaderStage.Pixel
        reads = [describe(textures, u.descriptor.resource) for u in state.GetReadOnlyResources(stage) if u.descriptor.resource in textures]
        writes = [describe(textures, o) for o in action.outputs if o != rd.ResourceId.Null()]
        writes += ["rw " + describe(textures, u.descriptor.resource) for u in state.GetReadWriteResources(stage)
                   if u.descriptor.resource in textures]
        say(action.eventId, "dispatch" if stage == rd.ShaderStage.Compute else "draw", action.numIndices, action.numInstances,
            "W", " ".join(writes), "R", " ".join(reads))
    for eid, rid in SAVE:
        if rid not in by_id:
            say("unknown resource", rid)
            continue
        controller.SetFrameEvent(eid, True)
        save = rd.TextureSave()
        save.resourceId = by_id[rid]
        save.destType = rd.FileType.EXR
        path = os.path.join(OUT, "res%d_eid%d.exr" % (rid, eid))
        controller.SaveTexture(save, path)
        say("saved", path)
    controller.Shutdown()
    cap.Shutdown()


try:
    main()
except Exception as error:
    say("fatal", repr(error))
log.close()
# qrenderdoc keeps its UI loop alive after a script returns.
os._exit(0)
