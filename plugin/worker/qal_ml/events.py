from __future__ import annotations

import json
import sys
from dataclasses import dataclass
from typing import Any, TextIO


EVENT_SCHEMA = "qal-ml-event/1.0"


@dataclass
class EventWriter:
    command: str
    stream: TextIO = sys.stdout

    def emit(
        self,
        event: str,
        phase: str,
        progress: float,
        message: str,
        data: dict[str, Any] | None = None,
    ) -> None:
        payload: dict[str, Any] = {
            "schema": EVENT_SCHEMA,
            "event": event,
            "command": self.command,
            "phase": phase,
            "progress": max(0.0, min(1.0, float(progress))),
            "message": message,
        }
        if data is not None:
            payload["data"] = data
        self.stream.write(json.dumps(payload, ensure_ascii=False, sort_keys=True) + "\n")
        self.stream.flush()


class NullEventWriter(EventWriter):
    def __init__(self, command: str = "internal") -> None:
        super().__init__(command=command)

    def emit(self, *args: Any, **kwargs: Any) -> None:
        return
