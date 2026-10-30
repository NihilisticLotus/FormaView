"""Rebuild FormaView's original vector mark and multi-resolution Windows icon."""
from pathlib import Path
from PIL import Image, ImageDraw

root = Path(__file__).resolve().parents[1] / 'src/host/icons'
# Three separated faces keep the model silhouette legible at taskbar sizes.
faces = [('#ffbd76', [(128,30),(214,80),(128,130),(42,80)]),
         ('#f28132', [(38,90),(123,139),(123,230),(38,181)]),
         ('#c9561c', [(133,139),(218,90),(218,181),(133,230)])]
svg = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256"><rect x="4" y="4" width="248" height="248" rx="52" fill="#242429"/>'
svg += ''.join(f'<polygon fill="{color}" points="'+ ' '.join(f'{x},{y}' for x,y in points)+'"/>' for color,points in faces) + '</svg>\n'
(root/'formaview.svg').write_text(svg,encoding='utf-8')
scale=4
image=Image.new('RGBA',(256*scale,256*scale))
draw=ImageDraw.Draw(image)
draw.rounded_rectangle((4*scale,4*scale,252*scale,252*scale),52*scale,fill='#242429')
for color,points in faces: draw.polygon([(x*scale,y*scale) for x,y in points],fill=color)
image=image.resize((256,256),Image.Resampling.LANCZOS)
image.save(root/'formaview.ico',sizes=[(n,n) for n in (16,20,24,32,40,48,64,128,256)])
