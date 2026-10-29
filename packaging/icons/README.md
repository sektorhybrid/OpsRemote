# OpsRemote icon

`opsremote.svg` is the source. The PNG sizes in `src/app/resources/icons/` and
the Windows icon `src/app/resources/opsremote.ico` are made from it:

```sh
for s in 16 24 32 48 64 128 256; do
    rsvg-convert -w $s -h $s packaging/icons/opsremote.svg \
        -o src/app/resources/icons/opsremote-$s.png
done
magick src/app/resources/icons/opsremote-{16,24,32,48,64,128,256}.png \
    src/app/resources/opsremote.ico
```
