"""Build FormaView's preview-and-export mark and multi-resolution Windows icon."""
from pathlib import Path
from PIL import Image, ImageDraw

root = Path(__file__).resolve().parents[1] / 'src/host/icons'
# A compact asset window carries the original folded diamond. The corner cuts
# read as a preview frame, while the right-facing facet suggests export.
faces = [('#f47c22', [('M',98,63),('L',130,91),('L',94,146),
                     ('L',69,119),('Z',)]),
         ('#ffbd76', [('M',128,57),('L',166,57),('L',187,95),
                     ('L',150,105),('L',119,66),('Z',)]),
         ('#c9561c', [('M',116,112),('L',151,129),('L',188,106),
                     ('L',169,145),('L',120,145),('Z',)]),
         ('#f28132', [('M',116,112),('L',150,105),('L',188,106),
                     ('L',164,126),('Z',)])]
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
svg = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256"><rect x="4" y="4" width="248" height="248" rx="52" fill="#242429"/><path d="M58 91V58h34M164 58h34v34M198 164v34h-34M92 198H58v-34" fill="none" stroke="#f4f0e8" stroke-width="12" stroke-linecap="round" stroke-linejoin="round"/>'
svg += ''.join(f'<path fill="{color}" d="{path_data(commands)}"/>' for color,commands in faces) + '</svg>\n'
(root/'formaview.svg').write_text(svg,encoding='utf-8')
scale=4
image=Image.new('RGBA',(256*scale,256*scale))
draw=ImageDraw.Draw(image)
draw.rounded_rectangle((4*scale,4*scale,252*scale,252*scale),52*scale,fill='#242429')
draw.line([(58*scale,91*scale),(58*scale,58*scale),(92*scale,58*scale)],fill='#f4f0e8',width=12*scale,joint='curve')
draw.line([(164*scale,58*scale),(198*scale,58*scale),(198*scale,92*scale)],fill='#f4f0e8',width=12*scale,joint='curve')
draw.line([(198*scale,164*scale),(198*scale,198*scale),(164*scale,198*scale)],fill='#f4f0e8',width=12*scale,joint='curve')
draw.line([(92*scale,198*scale),(58*scale,198*scale),(58*scale,164*scale)],fill='#f4f0e8',width=12*scale,joint='curve')
for color,commands in faces: draw.polygon([(x*scale,y*scale) for x,y in polygon(commands)],fill=color)
image=image.resize((256,256),Image.Resampling.LANCZOS)
image.save(root/'formaview.ico',sizes=[(n,n) for n in (16,20,24,32,40,48,64,128,256)])
