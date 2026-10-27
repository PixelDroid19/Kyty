# Attach to a process started with the RenderDoc layer enabled and capture the given presents.
# See docs/graphics-captures.md for the launch environment.
# Run with: qrenderdoc --python queue_capture.py ; configuration comes from the environment:
#   RDOC_FRAMES ("1100" or "1100,1103"), RDOC_TIMEOUT (seconds), RDOC_LOG, RDOC_STOP=1 to SIGTERM the target after.
import os
import signal
import time
import renderdoc as rd

log = open(os.environ["RDOC_LOG"], "w")


def say(*args):
    print(time.strftime("%H:%M:%S"), *args, file=log)
    log.flush()


def connect(deadline):
    while time.time() < deadline:
        ident = rd.EnumerateRemoteTargets("", 0)
        while ident != 0:
            target = rd.CreateTargetControl("", ident, "kyty-rdoc", True)
            if target is not None and target.GetTarget():
                return target
            ident = rd.EnumerateRemoteTargets("", ident)
        time.sleep(1.0)
    return None


def main():
    frames = sorted(int(f) for f in os.environ["RDOC_FRAMES"].split(","))
    deadline = time.time() + float(os.environ.get("RDOC_TIMEOUT", "300"))
    target = connect(deadline)
    if target is None:
        say("no target")
        return
    pid = target.GetPID()
    say("connected", target.GetTarget(), "pid", pid, "api", target.GetAPI())
    for frame in frames:
        target.QueueCapture(frame, 1)
    pending = len(frames)
    while pending > 0 and target.Connected() and time.time() < deadline:
        msg = target.ReceiveMessage(None)
        if msg.type == rd.TargetControlMessageType.NewCapture:
            say("capture frame", msg.newCapture.frameNumber, "path", msg.newCapture.path)
            pending -= 1
        elif msg.type == rd.TargetControlMessageType.Disconnected:
            break
    if pending > 0:
        say("missing", pending)
    target.Shutdown()
    if os.environ.get("RDOC_STOP") == "1" and pid != 0:
        os.kill(pid, signal.SIGTERM)
        say("stopped", pid)


try:
    main()
except Exception as error:
    say("fatal", repr(error))
log.close()
# qrenderdoc keeps its UI loop alive after a script returns.
os._exit(0)
