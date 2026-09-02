# About page QR assets

The About > Community tab loads this bundled runtime image:

- `romfs/img/community/qq.png`

The QQ card also displays the public LunarNX group number `736743823`. The
Discord card is generated at runtime from the permanent LunarNX invite URL
`https://discord.gg/cFZj8mpg2K`, so it does not require a checked-in image.

The About > Support author tab loads these bundled runtime images:

- `romfs/img/support/wechat.png`
- `romfs/img/support/alipay.png`

The payment assets are square PNG crops with a light quiet zone. The WeChat
asset masks only the account-name line; its center avatar is retained. Both
processed codes were checked against their source images to ensure the decoded
payload is unchanged. The UI uses nearest-neighbor interpolation and still
shows a labeled placeholder if an image is absent.
