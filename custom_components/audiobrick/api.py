"""Small async client for the Audio Brick's HTTP API.

Everything that only reads public state (`/status`) needs no password; everything that changes something, and the settings pages, need the web password,
which is sent as the `X-Token` header. See main/web/ in the firmware repository for the endpoints.
"""
from __future__ import annotations

import asyncio
from typing import Any

import aiohttp

REQUEST_TIMEOUT = aiohttp.ClientTimeout(total=8)


class AudioBrickError(Exception):
    """The Brick could not be reached or answered with an error."""


class AudioBrickAuthError(AudioBrickError):
    """The Brick refused the password (HTTP 401)."""


class AudioBrickClient:
    """Talks to one Audio Brick."""

    def __init__(self, session: aiohttp.ClientSession, host: str, password: str) -> None:
        self._session = session
        self._host = host
        self._password = password

    @property
    def host(self) -> str:
        return self._host

    @property
    def base_url(self) -> str:
        return f"http://{self._host}"

    async def _request(
        self,
        method: str,
        path: str,
        *,
        params: dict[str, Any] | None = None,
        data: bytes | str | None = None,
        want_json: bool = True,
    ) -> Any:
        headers = {"X-Token": self._password} if self._password else {}
        try:
            async with self._session.request(
                method, self.base_url + path, params=params, data=data, headers=headers, timeout=REQUEST_TIMEOUT
            ) as resp:
                if resp.status == 401:
                    raise AudioBrickAuthError("password refused")
                text = await resp.text()
                if resp.status >= 400:
                    raise AudioBrickError(text.strip() or f"HTTP {resp.status}")
                if not want_json:
                    return text
                return await resp.json(content_type=None) if text.strip().startswith(("{", "[")) else {}
        except (aiohttp.ClientError, asyncio.TimeoutError) as err:
            raise AudioBrickError(f"cannot reach {self._host}: {err}") from err

    # ---- reading ---------------------------------------------------------------------------------------------------

    async def status(self) -> dict[str, Any]:
        """Public state: no password needed."""
        return await self._request("GET", "/status")

    async def bluetooth(self) -> dict[str, Any]:
        return await self._request("GET", "/bluetooth")

    async def clips(self) -> list[str]:
        data = await self._request("GET", "/clips")
        return [f["name"] for f in data.get("files", [])]

    async def check_password(self) -> None:
        """Raise AudioBrickAuthError if the password is wrong (an endpoint that needs login)."""
        await self._request("GET", "/schedule")

    # ---- controlling -----------------------------------------------------------------------------------------------

    async def set_volume_level(self, level: float) -> None:
        await self._request("POST", "/volume", params={"level": f"{max(0.0, min(1.0, level)):.3f}"}, want_json=False)

    async def stop(self) -> None:
        await self._request("POST", "/media/stop", want_json=False)

    async def player(self, action: str) -> None:
        """action: play, pause, toggle, next, prev, stop."""
        await self._request("POST", f"/player/{action}", want_json=False)

    async def play_clip(self, name: str, loop: bool = False) -> None:
        params = {"name": name}
        if loop:
            params["loop"] = "1"
        await self._request("POST", "/clips/play", params=params, want_json=False)

    async def play_radio(self, url: str) -> None:
        await self._request("POST", "/radio/play", params={"url": url}, want_json=False)

    async def announce(self, audio: bytes) -> None:
        """Play an MP3 or 16-bit WAV (at most 400 KB) over whatever is playing."""
        await self._request("POST", "/announce", data=audio, want_json=False)

    async def set_ambient(self, on: bool) -> None:
        await self._request("POST", "/ambient/enable", params={"on": "1" if on else "0"}, want_json=False)

    async def set_bluetooth(self, on: bool) -> None:
        await self._request("POST", "/bluetooth", params={"on": "1" if on else "0"}, want_json=False)

    async def bluetooth_pairing(self, open_window: bool) -> None:
        await self._request("POST", "/bluetooth/pair", params={"on": "1" if open_window else "0"}, want_json=False)

    async def bluetooth_disconnect(self) -> None:
        await self._request("POST", "/bluetooth/disconnect", want_json=False)

    async def sleep_timer(self, minutes: int) -> None:
        await self._request("POST", "/schedule/sleep", params={"min": str(minutes)}, want_json=False)
