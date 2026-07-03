"""Clock switch: while on, the panel shows the current time, updated every minute."""
from __future__ import annotations

import logging
from typing import Any

from homeassistant.components.switch import SwitchEntity
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers.entity_platform import AddEntitiesCallback
from homeassistant.helpers.event import async_track_time_change
from homeassistant.helpers.restore_state import RestoreEntity
from homeassistant.util import dt as dt_util

from . import XyaoLedConfigEntry
from .const import MODE_STATIC
from .entity import XyaoLedEntity

_LOGGER = logging.getLogger(__name__)

CLOCK_FONT_SIZE = 16


async def async_setup_entry(
    hass: HomeAssistant,
    entry: XyaoLedConfigEntry,
    async_add_entities: AddEntitiesCallback,
) -> None:
    async_add_entities([XyaoLedClockSwitch(entry.runtime_data)])


class XyaoLedClockSwitch(XyaoLedEntity, SwitchEntity, RestoreEntity):
    """While on, renders HH:MM to the panel once a minute."""

    _attr_translation_key = "clock"
    _attr_assumed_state = True

    def __init__(self, panel) -> None:
        super().__init__(panel)
        self._attr_unique_id = f"{panel.address}-clock"
        self._attr_is_on = False
        self._unsub = None

    async def async_added_to_hass(self) -> None:
        await super().async_added_to_hass()
        last = await self.async_get_last_state()
        if last is not None and last.state == "on":
            self._attr_is_on = True
            self._arm()
            # First draw in the background: the panel may not be reachable
            # while HA is still starting; the next minute tick retries anyway.
            self.hass.async_create_task(self._tick())

    async def async_will_remove_from_hass(self) -> None:
        self._disarm()
        await super().async_will_remove_from_hass()

    def _arm(self) -> None:
        if self._unsub is None:
            self._unsub = async_track_time_change(self.hass, self._tick, second=0)

    def _disarm(self) -> None:
        if self._unsub is not None:
            self._unsub()
            self._unsub = None

    async def _tick(self, _now=None) -> None:
        try:
            await self._panel.display_text(
                dt_util.now().strftime("%H:%M"),
                self._panel.default_color,
                mode=MODE_STATIC,
                size=CLOCK_FONT_SIZE,
                speed=self._panel.default_speed,
            )
        except HomeAssistantError as err:
            _LOGGER.warning("Clock update failed (will retry next minute): %s", err)

    async def async_turn_on(self, **kwargs: Any) -> None:
        self._arm()
        self._attr_is_on = True
        self.async_write_ha_state()
        await self._tick()

    async def async_turn_off(self, **kwargs: Any) -> None:
        self._disarm()
        self._attr_is_on = False
        self.async_write_ha_state()
