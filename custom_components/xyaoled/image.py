"""Image entity: an upscaled preview of what was last sent to the panel."""
from __future__ import annotations

from homeassistant.components.image import ImageEntity
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from . import XyaoLedConfigEntry
from .entity import XyaoLedEntity
from .render import blank_preview_png


async def async_setup_entry(
    hass: HomeAssistant,
    entry: XyaoLedConfigEntry,
    async_add_entities: AddEntitiesCallback,
) -> None:
    async_add_entities([XyaoLedPreview(hass, entry.runtime_data)])


class XyaoLedPreview(XyaoLedEntity, ImageEntity):
    """Shows the panel's last rendered content (64x16, upscaled PNG)."""

    _attr_translation_key = "preview"
    _attr_content_type = "image/png"

    def __init__(self, hass: HomeAssistant, panel) -> None:
        XyaoLedEntity.__init__(self, panel)
        ImageEntity.__init__(self, hass)
        self._attr_unique_id = f"{panel.address}-preview"

    async def async_added_to_hass(self) -> None:
        await super().async_added_to_hass()
        if self._panel.preview is None:
            # Start with an empty panel instead of a broken-image placeholder.
            self._panel.preview = await self.hass.async_add_executor_job(blank_preview_png)

    @property
    def image_last_updated(self):
        return self._panel.preview_updated

    def image(self) -> bytes | None:
        return self._panel.preview
