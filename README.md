# Smart Wall Clock

## Building and deploying

To build and deploy the project you would need to define the following system variables:
- WIFI_SSID - WIFI access point name
- WIFI_PASS - WIFI password
- OTA_HOSTNAME - MDNS hostname for OTA
- OTA_PASS - OTA password

## Uploading
`pio run -t upload -e ota`

`pio run -t upload -e local`