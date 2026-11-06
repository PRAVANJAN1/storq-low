import asyncio
import inspect
import logging

from .event import WorkflowEvent

logger = logging.getLogger("storq.workflows.events")


class EventDispatcher:
    def __init__(self):
        self._handlers: dict[str, list] = {}

    def subscribe(self, event_type: str, handler=None):
        if handler is None:
            def decorator(fn):
                self._handlers.setdefault(event_type, []).append(fn)
                return fn
            return decorator
        self._handlers.setdefault(event_type, []).append(handler)
        return handler

    add_handler = subscribe

    def unsubscribe(self, event_type: str, handler) -> None:
        if event_type in self._handlers:
            self._handlers[event_type] = [
                item for item in self._handlers[event_type] if item != handler
            ]

    async def dispatch(self, event: WorkflowEvent) -> None:
        for handler in list(self._handlers.get(event.type, ())):
            try:
                result = handler(event)
                if inspect.isawaitable(result):
                    await result
            except Exception as error:
                logger.exception("workflow event handler failed for %s", event.type, exc_info=error)
