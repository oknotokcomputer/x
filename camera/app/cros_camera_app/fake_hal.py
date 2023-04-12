# Copyright 2023 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""This module encapsulates various tasks for Fake HAL setup."""

import json
import logging
import pathlib
import shutil
from typing import Callable, Dict, List, Optional, Tuple


_PERSISTENT_CONFIG_PATH = pathlib.Path("/etc/camera/fake_hal.json")
_CONFIG_PATH = pathlib.Path("/run/camera/fake_hal.json")

_RESOLUTIONS = [
    (320, 180),
    (320, 240),
    (640, 360),
    (640, 480),
    (1280, 720),
    (1280, 960),
    (1600, 1200),
    (1920, 1080),
    (2560, 1440),
    (2592, 1944),
    (3264, 2448),
    (3840, 2160),
]

_FPS_RANGES = [(15, 30), (30, 30), (15, 60), (60, 60)]

_EMPTY_CONFIG = {"cameras": []}


def _load_config() -> Dict:
    """Loads the config from JSON file.

    Returns:
        The deserialized config. If the config file does not exists, returns a
        valid empty config.
    """
    if not _CONFIG_PATH.exists():
        return _EMPTY_CONFIG

    with open(_CONFIG_PATH, encoding="utf-8") as f:
        return json.load(f)


def _save_config(config: Dict):
    """Saves the config to the JSON file.

    Args:
        config: The config to be saved.
    """
    # TODO(shik): Save config in a human-readable but concise JSON. Currently
    # the excessive line breaks for frame_rates field make it less pleasant.
    with open(_CONFIG_PATH, "w", encoding="utf-8") as f:
        json.dump(config, f, indent=2)


# (width, height, fps_range) -> keep_or_not
FormatFilter = Callable[[int, int, Tuple[int, int]], bool]


def _generate_supported_formats(should_keep: FormatFilter) -> List[Dict]:
    """Generates and filters supported formats.

    Args:
        should_keep: A predicate callback to select the supported formats.

    Returns:
        A list of supported formats that match the filter.
    """
    formats = []
    for w, h in _RESOLUTIONS:
        frame_rates = []
        for fps in _FPS_RANGES:
            if should_keep(w, h, fps):
                frame_rates.append(fps)

        if frame_rates:
            fmt = {
                "width": w,
                "height": h,
                "frame_rates": frame_rates,
            }
            formats.append(fmt)
    return formats


def _get_next_available_id(config: Dict) -> int:
    """Gets an available camera id from config.

    Args:
        config: The Fake HAL config.

    Returns:
        An unused valid camera id.
    """
    used_ids = set(c["id"] for c in config.get("cameras", []))
    next_id = 1
    while next_id in used_ids:
        next_id += 1
    return next_id


def persist():
    """Persists the config file for Fake HAL."""

    if _CONFIG_PATH.exists():
        logging.info(
            "Copy config from %s to %s", _CONFIG_PATH, _PERSISTENT_CONFIG_PATH
        )
        shutil.copy2(_CONFIG_PATH, _PERSISTENT_CONFIG_PATH)
    elif _PERSISTENT_CONFIG_PATH.exists():
        logging.info(
            "Remove persistent Fake HAL config %s", _PERSISTENT_CONFIG_PATH
        )
        _PERSISTENT_CONFIG_PATH.unlink()
    else:
        logging.info("No config found, nothing to persist.")


def add_camera(
    *,
    should_keep: FormatFilter,
    frame_path: Optional[pathlib.Path] = None,
):
    """Adds a new camera.

    Args:
        should_keep: A predicate callback to select the supported formats.
        frame_path: The source of camera frame in jpg, mjpg, or y4m format. If
            not specified, a test pattern would be used.
    """
    config = _load_config()
    camera_id = _get_next_available_id(config)

    new_camera = {
        "id": camera_id,
        "connected": True,
        "supported_formats": _generate_supported_formats(should_keep),
    }
    if frame_path is not None:
        new_camera["frames"] = {"path": frame_path}
    logging.debug("new_camera = %s", new_camera)
    config["cameras"].append(new_camera)

    connected = [c["id"] for c in config["cameras"] if c["connected"]]

    logging.info("Added camera with id = %d", camera_id)
    logging.info(
        "%d cameras in config with %d connected: %s",
        len(config["cameras"]),
        len(connected),
        connected,
    )

    _save_config(config)


def remove_cameras(should_remove: Callable[[int], bool]):
    """Removes specified cameras.

    Args:
        should_remove: A predicate callback to select cameras to remove.
    """
    config = _load_config()

    kept_cameras = []
    removed_cameras = []
    for c in config["cameras"]:
        if should_remove(c["id"]):
            removed_cameras.append(c)
        else:
            kept_cameras.append(c)
    logging.info(
        "Removed %d camera(s): %s",
        len(removed_cameras),
        [c["id"] for c in removed_cameras],
    )
    logging.info(
        "Kept %d camera(s): %s",
        len(kept_cameras),
        [c["id"] for c in kept_cameras],
    )

    config["cameras"] = kept_cameras
    _save_config(config)
