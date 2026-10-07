"""The Brick as a media player: stored clips, internet radio, announcements (text to speech), volume, transport buttons."""
from __future__ import annotations

from typing import Any

import aiohttp

from homeassistant.components import media_source
from homeassistant.components.media_player import (
    BrowseMedia,
    MediaClass,
    MediaPlayerEntity,
    MediaPlayerEntityFeature,
    MediaPlayerState,
    MediaType,
    async_process_play_media_url,
)
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from . import AudioBrickConfigEntry
from .api import AudioBrickError
from .const import ANNOUNCE_MAX_BYTES
from .coordinator import AudioBrickCoordinator
from .entity import AudioBrickEntity


async def async_setup_entry(hass: HomeAssistant, entry: AudioBrickConfigEntry, async_add_entities: AddConfigEntryEntitiesCallback) -> None:
    async_add_entities([AudioBrickMediaPlayer(entry.runtime_data.coordinator)])


class AudioBrickMediaPlayer(AudioBrickEntity, MediaPlayerEntity):
    """Plays what is stored on the Brick or streams from the network; the amp volume is the player volume."""

    _attr_name = None  # the device name
    _attr_media_content_type = MediaType.MUSIC

    def __init__(self, coordinator: AudioBrickCoordinator) -> None:
        super().__init__(coordinator, "media_player")
        features = (
            MediaPlayerEntityFeature.PAUSE
            | MediaPlayerEntityFeature.PLAY
            | MediaPlayerEntityFeature.STOP
            | MediaPlayerEntityFeature.VOLUME_SET
            | MediaPlayerEntityFeature.VOLUME_STEP
        )
        flags = coordinator.data["status"].get("features", {})
        if flags.get("clips"):
            features |= (
                MediaPlayerEntityFeature.NEXT_TRACK
                | MediaPlayerEntityFeature.PREVIOUS_TRACK
                | MediaPlayerEntityFeature.PLAY_MEDIA
                | MediaPlayerEntityFeature.BROWSE_MEDIA
            )
        if flags.get("announce"):
            features |= MediaPlayerEntityFeature.PLAY_MEDIA | MediaPlayerEntityFeature.MEDIA_ANNOUNCE
        self._attr_supported_features = features

    # ---- state -----------------------------------------------------------------------------------------------------

    @property
    def state(self) -> MediaPlayerState:
        player = self.status.get("player", {})
        media = self.status.get("media", {})
        if media.get("src", "none") != "none" or player.get("state") == "playing":
            return MediaPlayerState.PLAYING
        if player.get("state") == "paused":
            return MediaPlayerState.PAUSED
        return MediaPlayerState.IDLE

    @property
    def volume_level(self) -> float | None:
        return self.status.get("volume_level")

    @property
    def media_title(self) -> str | None:
        label = self.status.get("media", {}).get("label")
        return label or self.status.get("player", {}).get("clip") or None

    @property
    def extra_state_attributes(self) -> dict[str, Any]:
        media = self.status.get("media", {})
        return {
            "source_kind": media.get("src"),
            "volume_db": self.status.get("vol_db"),
            "amp": self.status.get("amp"),
            "overlay": media.get("overlay") or None,
        }

    # ---- control ---------------------------------------------------------------------------------------------------

    async def _do(self, call) -> None:
        try:
            await call
        except AudioBrickError as err:
            raise HomeAssistantError(str(err)) from err
        await self.coordinator.async_request_refresh()

    async def async_set_volume_level(self, volume: float) -> None:
        await self._do(self.coordinator.client.set_volume_level(volume))

    async def async_media_play(self) -> None:
        await self._do(self.coordinator.client.player("play"))

    async def async_media_pause(self) -> None:
        await self._do(self.coordinator.client.player("pause"))

    async def async_media_stop(self) -> None:
        await self._do(self.coordinator.client.stop())

    async def async_media_next_track(self) -> None:
        await self._do(self.coordinator.client.player("next"))

    async def async_media_previous_track(self) -> None:
        await self._do(self.coordinator.client.player("prev"))

    async def async_play_media(self, media_type: MediaType | str, media_id: str, **kwargs: Any) -> None:
        """media_id: a clip name, an http(s) address of an MP3 radio stream, a media-source:// item, or (with announce) any audio address."""
        announce = bool(kwargs.get("announce"))
        if media_source.is_media_source_id(media_id):
            item = await media_source.async_resolve_media(self.hass, media_id, self.entity_id)
            media_id = async_process_play_media_url(self.hass, item.url)
            announce = announce or True  # a resolved item is a file: it can only be played as an announcement
        elif media_id.startswith("/"):
            media_id = async_process_play_media_url(self.hass, media_id)
            announce = True

        if announce:
            audio = await self._download(media_id)
            await self._do(self.coordinator.client.announce(audio))
        elif media_id.startswith(("http://", "https://")):
            await self._do(self.coordinator.client.play_radio(media_id))
        else:
            await self._do(self.coordinator.client.play_clip(media_id))

    async def _download(self, url: str) -> bytes:
        session = async_get_clientsession(self.hass)
        try:
            async with session.get(url, timeout=aiohttp.ClientTimeout(total=30)) as resp:
                if resp.status != 200:
                    raise HomeAssistantError(f"could not fetch the audio ({resp.status})")
                data = await resp.content.read(ANNOUNCE_MAX_BYTES + 1)
        except aiohttp.ClientError as err:
            raise HomeAssistantError(f"could not fetch the audio: {err}") from err
        if len(data) > ANNOUNCE_MAX_BYTES:
            raise HomeAssistantError("the announcement is longer than 400 KB (about 25 s of MP3); the Brick cannot play it")
        return data

    # ---- browsing the stored clips ---------------------------------------------------------------------------------

    async def async_browse_media(self, media_content_type: str | None = None, media_content_id: str | None = None) -> BrowseMedia:
        if media_content_id and media_source.is_media_source_id(media_content_id):
            return await media_source.async_browse_media(self.hass, media_content_id)
        children = [
            BrowseMedia(
                title=name,
                media_class=MediaClass.MUSIC,
                media_content_id=name,
                media_content_type=MediaType.MUSIC,
                can_play=True,
                can_expand=False,
            )
            for name in self.coordinator.data["clips"]
        ]
        return BrowseMedia(
            title="Clips on the Brick",
            media_class=MediaClass.DIRECTORY,
            media_content_id="",
            media_content_type="library",
            can_play=False,
            can_expand=True,
            children=children,
        )
