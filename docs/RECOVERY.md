# If something goes wrong

Work down this list: each step is for an FM-1 the one before cannot reach.

## 1. The install stopped halfway

The FM-1 stays in its **update loader** (dark screen, or UPDATING). Plug it back in, open the flasher (the companion's **Firmware…** button, on a computer),
press **Find the FM-1**: it says "the update loader", and **Resume** finishes the install. Nothing else is needed; the
boot area was never touched.

## 2. ECHOFIELD (or another firmware) does not start

After two failed starts the FM-1 opens **SAFE MODE**: no sound, USB on. The flasher finds it: install ECHOFIELD again,
or the stock firmware. In SAFE MODE, PLAY tries the firmware once more.

## 3. Back to M-VAVE's firmware

Download M-VAVE's FM-1 firmware (m-vave.com → Download → PC Firmware), then either the flasher's
**Back to the stock firmware**, or M-VAVE's own updater **M-UPGRADE**. Samples and presets kept by ECHOFIELD may be lost:
back them up first (Samples → Backup).

## 4. Nothing answers on USB

After four failed starts the chip enters its own update mode (the mask ROM's "UBOOT": it appears on USB as
`WL80UBOOT1.00`, 4C4A:8057). From a Mac or Linux, felucca-bsp's rescue script puts stock firmware back with no extra
hardware:

    python3 -m venv ~/fm1-rescue && ~/fm1-rescue/bin/pip install pyusb libusb-package
    sudo ~/fm1-rescue/bin/python base/bsp/tools/fm1_rescue.py V15.fwsc V16.fwsc            # checks, backs up, writes nothing
    sudo ~/fm1-rescue/bin/python base/bsp/tools/fm1_rescue.py V15.fwsc V16.fwsc --write    # then writes stock

It reads and saves the whole flash before anything, never writes the boot area, writes only the sectors that differ
and reads everything back. Give it the stock packages you have; it picks the one matching the FM-1's boot area.

## 5. Last resort

[FM-1-transporter](https://github.com/kurogedelic/FM-1-transporter) with an RP2040 board reads and writes the whole
flash. Not needed so far by any FM-1 running a firmware built on felucca-bsp's update path.
