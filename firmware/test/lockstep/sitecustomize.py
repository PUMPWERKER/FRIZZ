"""sitecustomize.py: a tool of firmware/ on the virtual CHOMPI's clock (lockstep, #388).

ptytwin.h's Exec puts this folder on PYTHONPATH, and FRIZZ_LOCKSTEP names the Unix socket it
listens on; Python runs this file at its start. Nothing else does: on the CHOMPI the tools keep
the wall clock, and without FRIZZ_LOCKSTEP (and FRIZZ_NO_DEVICE, which only the host checks
set) this file does nothing.

On the twin's clock, every way the tools have of telling or waiting for the time runs on the
twin's: time.monotonic / perf_counter / time and their _ns, time.sleep, select.select on the
twin's pseudo-terminals, threading's Lock, RLock and Condition (so Event, Semaphore ...),
Thread.join, and Popen.wait on a Python child (which takes the same clock). The time is the
twin's, BASE + its ms / 1000 (ptytwin.h's kLockstepBase), and stands still while anything here
runs. The twin advances only when every thread of every process on it waits, to the earliest
time one waits for, or until it sends something a thread waits for: a stall anywhere (a busy
computer) slows everything together, and the timing comes out the same under any load.

Each process counts its threads that run (the main thread, each Thread from its start to the
end of its run). One that waits takes itself off; one that wakes another (a notify, a lock let
go, a thread's end) puts it back on before it goes on, so the count never reaches 0 between the
two. At 0 the process tells the twin IDLE, with the earliest time it waits for and whether a
thread waits for the twin's bytes. The twin then advances and sends NOW, one process at a time;
the process wakes the threads that are due one at a time, each running until every thread waits
again: the order is the same in every run.

The protocol, lines on the socket: HELLO pid (answered TIME ms), IDLE ms|- 0|1, BUSY (a signal
woke a thread), CHILD pid (answered UP pid once that child said HELLO); from the twin NOW ms.
Two threads of its own, not counted, talk to the twin: one reads its lines, one runs the rounds.
SIGUSR1 prints every thread's stack (faulthandler): the twin's watchdog asks for it when it's
stuck.
"""
import os
import sys

BASE = 1000.  # s: the twin's ms 0 (ptytwin.h's kLockstepBase)
PYTHONS = ((3, 12), (3, 13), (3, 14))  # the versions whose threading and subprocess this was checked on


def _install(addr):
    import _thread
    import collections
    import faulthandler
    import math
    import select
    import signal
    import socket
    import time

    INF = math.inf
    real_select = select.select
    real_sleep = time.sleep
    RealLock = _thread.allocate_lock
    epoch = float(os.environ.get("FRIZZ_LOCKSTEP_EPOCH", "0"))
    twin_rdevs = set()
    for name in ("FRIZZ_LOCKSTEP_USB", "FRIZZ_LOCKSTEP_TRS"):
        try:
            twin_rdevs.add(os.stat(os.environ[name]).st_rdev)
        except (KeyError, OSError):
            pass

    class Waiter:
        __slots__ = ("deadline", "fds", "lock", "notified")

        def __init__(self, deadline, fds):
            self.deadline, self.fds, self.notified = deadline, fds, False
            self.lock = RealLock()
            self.lock.acquire()

    def ready(fds):
        try:
            return bool(real_select(fds, [], [], 0)[0])
        except (OSError, ValueError):
            return True  # closed: the thread finds out itself

    class Sched:
        def __init__(self):
            self.lock = RealLock()
            self.settled = RealLock()  # released when the count reaches 0 during a round
            self.settled.acquire()
            self.round = False
            self.running = 1  # the main thread
            self.waiters = []  # in the order they came
            self.reads = 0  # time read since the last wait: a loop that spins on it
            self.up = set()
            self.twin_fds = set()
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.connect("\0" + addr[1:] if addr.startswith("@") else addr)
            # the bare fd, which nothing closes but the process's end: a socket object would be
            # closed when Python tears its modules down, and the twin would take that for the end
            # while the process still runs
            self.fd = sock.detach()
            self.buf = b""
            self.send("HELLO %d" % os.getpid())
            cmd, ms = self.readline().split()
            if cmd != "TIME":
                raise SystemExit("lockstep: the twin answered %r" % cmd)
            self.ms = int(ms)
            self.next_ms = None
            self.now_came = RealLock()  # released when a NOW came, for the rounds' thread
            self.now_came.acquire()
            # two threads of its own, not counted: one reads the twin's lines, one wakes the
            # threads due (a round waits for them, while an UP may come meanwhile)
            _thread.start_new_thread(self.listen, ())
            _thread.start_new_thread(self.rounds, ())

        def now(self):
            return BASE + self.ms / 1000.

        def send(self, line):
            data = (line + "\n").encode()
            while data:
                data = data[os.write(self.fd, data):]

        def readline(self):
            while b"\n" not in self.buf:
                data = os.read(self.fd, 4096)
                if not data:
                    return None
                self.buf += data
            line, self.buf = self.buf.split(b"\n", 1)
            return line.decode()

        # --- with self.lock held ---
        def idle(self):
            deadline = min((w.deadline for w in self.waiters), default=INF)
            io = any(w.fds for w in self.waiters)
            self.send("IDLE %s %d" % ("-" if deadline == INF else repr((deadline - BASE) * 1000.), io))

        def off(self):
            """A thread stops running: waits, or its run ended"""
            self.running -= 1
            self.reads = 0
            if self.running == 0:
                if self.round:
                    if self.settled.locked():
                        self.settled.release()
                else:
                    self.idle()

        def register(self, deadline, fds=None, queue=None, off=True):
            """A waiter for this thread, the thread off unless off is False: then the caller
            takes it off (off()) once it has let go of what another may be waiting for"""
            w = Waiter(deadline, fds)
            if queue is not None:
                queue.append(w)
            self.waiters.append(w)
            if off:
                self.off()
            return w

        def wake(self, w, notified):
            """w on again, if it still waits: True then"""
            if not any(x is w for x in self.waiters):
                return False
            self.waiters.remove(w)
            w.notified = notified
            self.running += 1
            w.lock.release()
            return True

        # --- without ---
        def block(self, w):
            """Until w is woken: True if by a notify (a lock handed over, a thread's end), False
            by its time or its fds"""
            try:
                w.lock.acquire()
            except BaseException:
                # a signal's exception (Ctrl-C, SIGTERM): this thread runs again, the twin waits
                with self.lock:
                    if any(x is w for x in self.waiters):
                        self.waiters.remove(w)
                        self.running += 1
                        self.send("BUSY")
                raise
            return w.notified

        def wait(self, deadline, fds=None):
            """Until the twin's time deadline (s), or fds (a select's) are readable"""
            with self.lock:
                if deadline <= self.now():
                    return False
                w = self.register(deadline, fds)
            return self.block(w)

        def listen(self):
            while True:
                line = self.readline()
                if line is None:
                    sys.stderr.write("lockstep: the twin's clock is gone\n")
                    os._exit(75)
                cmd, arg = line.split()
                if cmd == "NOW":
                    if not self.now_came.locked():
                        # one at a time, the next only after IDLE: else this process said IDLE
                        # while a thread of it ran, and its timing can't be trusted
                        sys.stderr.write("lockstep: NOW %s before the round at %d ended\n" % (arg, self.ms))
                        os._exit(70)
                    self.next_ms = int(arg)
                    self.now_came.release()
                elif cmd == "UP":
                    with self.lock:
                        self.up.add(int(arg))

        def rounds(self):
            while True:
                self.now_came.acquire()
                self.ms = self.next_ms
                self.wake_due()

        def wake_due(self):
            """The threads due now, one at a time, each round until every thread waits again"""
            now = self.now()
            with self.lock:
                self.round = True
                while True:
                    due = next((w for w in self.waiters if w.deadline <= now or (w.fds and ready(w.fds))), None)
                    if due is None:
                        break
                    self.wake(due, False)
                    if self.running:
                        self.lock.release()
                        self.settled.acquire()
                        self.lock.acquire()
                self.round = False
                if self.running == 0:
                    self.idle()

        def read_time(self):
            # a loop that reads the time and never waits would spin for ever on a clock that stands
            # still: after very many reads, it waits a ms
            self.reads += 1
            if self.reads > 200000:
                if self.reads == 200001:
                    sys.stderr.write("lockstep: a loop spins on the clock without waiting; 1 ms at a time\n")
                self.wait(self.now() + .001)
            return self.now()

    S = Sched()

    # --- time ---
    def monotonic():
        return S.read_time()

    def wall():
        return epoch + S.read_time() - BASE

    def sleep(s):
        if s < 0:
            raise ValueError("sleep length must be non-negative")
        S.wait(S.now() + s)

    time.monotonic = time.perf_counter = monotonic
    time.monotonic_ns = time.perf_counter_ns = lambda: int(monotonic() * 1e9)
    time.time = wall
    time.time_ns = lambda: int(wall() * 1e9)
    time.sleep = sleep

    # --- the twin's pseudo-terminals: os.open notes them, select waits on them ---
    real_open, real_close = os.open, os.close

    def vopen(path, flags, mode=0o777, *, dir_fd=None):
        fd = real_open(path, flags, mode, dir_fd=dir_fd)
        try:
            if twin_rdevs and os.fstat(fd).st_rdev in twin_rdevs:
                S.twin_fds.add(fd)
        except OSError:
            pass
        return fd

    def vclose(fd):
        S.twin_fds.discard(fd)
        real_close(fd)

    os.open, os.close = vopen, vclose

    def fileno(f):
        return f if isinstance(f, int) else f.fileno()

    def vselect(rlist, wlist, xlist, timeout=None):
        if wlist or xlist or not S.twin_fds.intersection(fileno(f) for f in rlist):
            return real_select(rlist, wlist, xlist, timeout)
        deadline = INF if timeout is None else S.now() + timeout
        while True:
            got = real_select(rlist, wlist, xlist, 0)
            if got[0] or S.now() >= deadline:
                return got
            S.wait(deadline, list(rlist))

    select.select = vselect

    # --- threading: locks, conditions, threads ---
    import threading

    threading._time = monotonic

    class VLock:
        """threading.Lock on the twin's clock: a thread that waits for it waits as any other,
        and the one that lets it go hands it over"""

        def __init__(self):
            self._lock = RealLock()
            self._queue = collections.deque()

        def acquire(self, blocking=True, timeout=-1):
            with S.lock:
                if self._lock.acquire(False):
                    return True
                if not blocking:
                    return False
                deadline = INF if timeout is None or timeout < 0 else S.now() + timeout
                if deadline <= S.now():
                    return False
                w = S.register(deadline, queue=self._queue)
            try:
                handed = S.block(w)
            except BaseException:
                if w.notified:
                    self.release()
                raise
            if not handed:
                with S.lock:
                    if w in self._queue:
                        self._queue.remove(w)
            return handed

        def release(self):
            with S.lock:
                while self._queue:
                    if S.wake(self._queue.popleft(), True):
                        return  # still held, now by the one woken
                self._lock.release()

        def locked(self):
            return self._lock.locked()

        def __enter__(self):
            return self.acquire()

        def __exit__(self, *exc):
            self.release()

        def _at_fork_reinit(self):
            self._lock = RealLock()
            self._queue.clear()

    class VRLock:
        def __init__(self):
            self._block = VLock()
            self._owner, self._count = None, 0

        def acquire(self, blocking=True, timeout=-1):
            me = _thread.get_ident()
            if self._owner == me:
                self._count += 1
                return True
            if not self._block.acquire(blocking, timeout):
                return False
            self._owner, self._count = me, 1
            return True

        def release(self):
            if self._owner != _thread.get_ident():
                raise RuntimeError("cannot release un-acquired lock")
            self._count -= 1
            if not self._count:
                self._owner = None
                self._block.release()

        __enter__ = acquire

        def __exit__(self, *exc):
            self.release()

        def _is_owned(self):
            return self._owner == _thread.get_ident()

        def _release_save(self):
            state = self._count, self._owner
            self._owner, self._count = None, 0
            self._block.release()
            return state

        def _acquire_restore(self, state):
            self._block.acquire()
            self._count, self._owner = state

        def _at_fork_reinit(self):
            self._block._at_fork_reinit()
            self._owner, self._count = None, 0

    class VCondition:
        """threading.Condition on the twin's clock (so Event, Semaphore, Barrier ...)"""

        def __init__(self, lock=None):
            self._lock = VRLock() if lock is None else lock
            self.acquire, self.release = self._lock.acquire, self._lock.release
            self._waiters = collections.deque()

        def __enter__(self):
            return self._lock.__enter__()

        def __exit__(self, *exc):
            return self._lock.__exit__(*exc)

        def _release_save(self):
            if hasattr(self._lock, "_release_save"):
                return self._lock._release_save()
            self._lock.release()

        def _acquire_restore(self, state):
            if hasattr(self._lock, "_acquire_restore"):
                self._lock._acquire_restore(state)
            else:
                self._lock.acquire()

        def _is_owned(self):
            if hasattr(self._lock, "_is_owned"):
                return self._lock._is_owned()
            if self._lock.acquire(False):
                self._lock.release()
                return False
            return True

        def _at_fork_reinit(self):
            self._lock._at_fork_reinit()
            self._waiters.clear()

        def wait(self, timeout=None):
            if not self._is_owned():
                raise RuntimeError("cannot wait on un-acquired lock")
            if timeout is not None and timeout <= 0:
                self._acquire_restore(self._release_save())
                return False
            # off only once the lock is let go: letting it go can hand it to a thread that waits
            # for it, which runs then; off before, the process could say IDLE while that one runs
            with S.lock:
                w = S.register(INF if timeout is None else S.now() + timeout, queue=self._waiters, off=False)
            state = self._release_save()
            with S.lock:
                S.off()
            notified = False
            try:
                notified = S.block(w)
                return notified
            finally:
                self._acquire_restore(state)
                if not notified and w in self._waiters:
                    self._waiters.remove(w)

        def wait_for(self, predicate, timeout=None):
            end = None
            result = predicate()
            while not result:
                if timeout is not None:
                    if end is None:
                        end = S.now() + timeout
                    left = end - S.now()
                    if left <= 0:
                        break
                    self.wait(left)
                else:
                    self.wait()
                result = predicate()
            return result

        def notify(self, n=1):
            if not self._is_owned():
                raise RuntimeError("cannot notify on un-acquired lock")
            while n > 0 and self._waiters:
                w = self._waiters.popleft()
                with S.lock:
                    if S.wake(w, True):
                        n -= 1

        def notify_all(self):
            self.notify(len(self._waiters))

        notifyAll = notify_all

    threading.Lock, threading.RLock, threading.Condition = VLock, VRLock, VCondition

    real_start, real_join = threading.Thread.start, threading.Thread.join

    def start(self):
        run = self.run
        self._lockstep_end = []  # joiners' waiters, until it ended: None

        def counted():
            try:
                run()
            finally:
                with S.lock:
                    for w in self._lockstep_end:
                        S.wake(w, True)
                    self._lockstep_end = None
                    S.off()

        self.run = counted
        with S.lock:
            S.running += 1
        try:
            real_start(self)
        except BaseException:
            with S.lock:
                S.off()
            raise

    def join(self, timeout=None):
        if getattr(self, "_lockstep_end", None) is not None and self is not threading.current_thread():
            deadline = INF if timeout is None else S.now() + max(timeout, 0.)
            while True:
                with S.lock:
                    if self._lockstep_end is None:
                        break
                    if S.now() >= deadline:
                        return
                    w = S.register(deadline, queue=self._lockstep_end)
                S.block(w)
        real_join(self)

    threading.Thread.start, threading.Thread.join = start, join
    real_shutdown = threading._shutdown

    def shutdown():
        # the threads Python waits for at its end (not daemons), joined on the twin's clock first:
        # its own join waits on locks that don't know it
        for t in threading.enumerate():
            if t is not threading.current_thread() and not t.daemon:
                t.join()
        real_shutdown()

    threading._shutdown = shutdown

    # --- a Python child runs on the same clock: the twin waits for its HELLO ---
    import subprocess

    subprocess._time = monotonic
    real_init, real_wait = subprocess.Popen.__init__, subprocess.Popen.wait

    def init(self, args, *a, **kw):
        real_init(self, args, *a, **kw)
        # Python, or a Python script, maybe behind a command that execs it (setpriv, reaper.py's)
        env = kw.get("env") if kw.get("env") is not None else os.environ
        words = [args] if isinstance(args, (str, bytes, os.PathLike)) else list(args)
        words = [os.fsdecode(w) for w in [kw.get("executable") or words[0]] + words[1:]]
        self._lockstep = (not kw.get("shell") and "FRIZZ_LOCKSTEP" in env
                          and any(os.path.basename(w).startswith("python") or w.endswith(".py") for w in words))
        if self._lockstep:
            with S.lock:
                S.send("CHILD %d" % self.pid)
            while self.pid not in S.up and self.poll() is None:
                real_sleep(.001)

    def wait(self, timeout=None):
        if not getattr(self, "_lockstep", False):
            return real_wait(self, timeout)
        deadline = INF if timeout is None else S.now() + timeout
        while self.poll() is None:
            if S.now() >= deadline:
                raise subprocess.TimeoutExpired(self.args, timeout)
            S.wait(min(deadline, S.now() + .001))
        return self.returncode

    subprocess.Popen.__init__, subprocess.Popen.wait = init, wait

    faulthandler.register(signal.SIGUSR1, all_threads=True)


_addr = os.environ.get("FRIZZ_LOCKSTEP")
if _addr and os.environ.get("FRIZZ_NO_DEVICE"):
    if sys.version_info[:2] not in PYTHONS:
        sys.exit("lockstep: Python %d.%d isn't one this was checked on (%s): FRIZZ_TWIN_CLOCK=0 runs the "
                 "twin's checks on the wall clock" % (sys.version_info[:2] + (
                     ", ".join("%d.%d" % v for v in PYTHONS),)))
    _install(_addr)
