# Wi-Fi list and SoftAP provisioning

The showcase keeps a local list of saved networks and adds new ones through a
temporary SoftAP portal shown on the e-paper panel. No companion app, NFC write
tool or cloud service is required.

## Model

- **Saved Networks** — select a stored profile to connect, or delete it.
- **Add Network** — the device opens a SoftAP, prints SSID / password / URL on
  the panel, and serves a phone browser form that scans nearby 2.4 GHz APs.

Profiles are stored in NVS namespace `zwifi` (up to eight SSID/password pairs).

## SoftAP add flow

1. Choose **WIFI → ADD NETWORK**.
2. Join the on-screen hotspot (name `NOTE4-XXXX`, random 8-character password).
3. Scan the on-screen QR code (encodes `http://192.168.4.1`), or type that URL.
4. Select a network (or type an SSID), enter the password and submit.
5. The device saves the profile, stops the SoftAP and connects as STA.
6. Hold OK during the waiting screen to cancel.

The ADD WIFI panel uses Design A: numbered steps on the left, portal QR on the
right. Regenerate the embedded bitmap with:

```bash
python tools/generate_portal_qr.py
```

The portal times out after five minutes.

## Controls

| Screen | Input | Action |
| --- | --- | --- |
| Wi-Fi menu | OK | Open saved list or add flow |
| Saved list | OK | Connect / delete submenu |
| Add portal | Hold OK | Cancel SoftAP session |

## Component

Implementation lives in `components/zectrix_wifi`. The English UI screens are in
`components/zectrix_demo_ui` (`ShowWifiPortal`, `ShowWifiStatus`).
