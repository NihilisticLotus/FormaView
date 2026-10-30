"""Rebuild FormaView's original vector mark and multi-resolution Windows icon."""
from pathlib import Path
from PIL import Image, ImageDraw

root = Path(__file__).resolve().parents[1] / 'src/host/icons'
# The selected folded-ribbon mark: a triangular opening and three broad planes.
# Shared path commands keep SVG and raster silhouettes identical.
faces = [('#f47c22', [('M',91,31),('Q',96,26,100,34),('L',131,86),
                     ('L',60,193),('L',33,147),('Q',27,137,33,127),('Z',)]),
         ('#ffbd76', [('M',108,28),('L',165,28),('Q',177,28,183,39),
                     ('L',229,122),('Q',235,132,230,141),('L',171,149),
                     ('L',104,35),('Q',100,28,108,28),('Z',)]),
         ('#c9561c', [('M',105,137),('L',174,175),('L',230,141),
                     ('L',203,193),('Q',196,208,183,208),('L',81,208),
                     ('Q',65,208,69,196),('Z',)]),
         ('#f28132', [('M',105,137),('L',171,149),('L',230,141),
                     ('L',199,181),('Q',195,188,184,182),('Z',)])]
def path_data(commands):
    return ' '.join(str(value) for command in commands for value in command)
def polygon(commands):
    points=[]
    for op,*v in commands:
        if op in ('M','L'): points.append(tuple(v))
        elif op=='Q':
            a=points[-1];b=v[:2];c=v[2:]
            for i in range(1,33):
                t=i/32
                points.append(tuple((1-t)**2*a[j]+2*(1-t)*t*b[j]+t*t*c[j] for j in (0,1)))
    return points
svg = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256"><rect x="4" y="4" width="248" height="248" rx="52" fill="#242429"/>'
svg += ''.join(f'<path fill="{color}" d="{path_data(commands)}"/>' for color,commands in faces) + '</svg>\n'
(root/'formaview.svg').write_text(svg,encoding='utf-8')
scale=4
image=Image.new('RGBA',(256*scale,256*scale))
draw=ImageDraw.Draw(image)
draw.rounded_rectangle((4*scale,4*scale,252*scale,252*scale),52*scale,fill='#242429')
for color,commands in faces: draw.polygon([(x*scale,y*scale) for x,y in polygon(commands)],fill=color)
image=image.resize((256,256),Image.Resampling.LANCZOS)
image.save(root/'formaview.ico',sizes=[(n,n) for n in (16,20,24,32,40,48,64,128,256)])
