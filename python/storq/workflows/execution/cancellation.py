import threading


class CancellationManager:
    def __init__(self):
        self._cancelled = False
        self._event = threading.Event()
        self._uncancellable_steps: set[str] = set()

    def request(self) -> None:
        self._cancelled = True
        self._event.set()

    @property
    def requested(self) -> bool:
        return self._cancelled

    def mark_uncancellable(self, step_name: str) -> None:
        self._uncancellable_steps.add(step_name)

    @property
    def uncancellable_steps(self) -> set[str]:
        return set(self._uncancellable_steps)
