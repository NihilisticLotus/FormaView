"""Rebuild FormaView's original vector mark and multi-resolution Windows icon."""
from pathlib import Path
from PIL import Image, ImageDraw

root = Path(__file__).resolve().parents[1] / 'src/host/icons'
faces = [('#a84312', [(76,60),(104,42),(104,194),(76,214)]),
         ('#ffbd76', [(76,60),(176,60),(204,42),(104,42)]),
         ('#f28132', [(76,60),(176,60),(176,94),(112,94),(112,120),(164,120),(164,154),(112,154),(112,194),(76,214)]),
         ('#cf5619', [(176,60),(204,42),(204,76),(176,94)]),
         ('#ffbd76', [(112,120),(164,120),(192,102),(140,102)]),
         ('#b74c16', [(164,120),(192,102),(192,136),(164,154)])]
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
