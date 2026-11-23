"use strict";
// P6 captures emitted by MHF_GL_DUMP.
function frame(bytes) {
  let pos = 0;
  const space = b => [9,10,11,12,13,32].includes(b);
  function token() {
    while (pos < bytes.length) {
      if (space(bytes[pos])) { pos++; continue; }
      if (bytes[pos] !== 35) break;
      while (pos < bytes.length && bytes[pos] !== 10) pos++;
    }
    const start = pos;
    while (pos < bytes.length && !space(bytes[pos]) && bytes[pos] !== 35) pos++;
    return new TextDecoder().decode(bytes.subarray(start,pos));
  }
  if (token() !== "P6") throw Error("Expected a binary P6 PPM");
  const w = Number(token()), h = Number(token());
  if (!Number.isSafeInteger(w)||!Number.isSafeInteger(h)||w<1||h<1||token()!=="255"||!space(bytes[pos])) throw Error("Invalid PPM header");
  if (bytes[pos++]===13 && bytes[pos]===10) pos++;
  const pixels = bytes.subarray(pos);
  if (pixels.length!==w*h*3) throw Error("Incomplete or oversized PPM raster");
  return {w,h,pixels};
}
function difference(a,b) {
  if(a.w!==b.w || a.h!==b.h) return {status:"Different dimensions",sizes:[[a.w,a.h],[b.w,b.h]]};
  let count=0, left=a.w,top=a.h,right=-1,bottom=-1;
  for(let i=0;i<a.pixels.length;i+=3){
    if(a.pixels[i]===b.pixels[i] && a.pixels[i+1]===b.pixels[i+1] && a.pixels[i+2]===b.pixels[i+2]) continue;
    const x=(i/3)%a.w,y=Math.floor(i/3/a.w);
    count++; left=Math.min(left,x); right=Math.max(right,x); top=Math.min(top,y); bottom=Math.max(bottom,y);
  }
  return count?{status:"Changed",count,bounds:[left,top,right,bottom]}:{status:"Identical"};
}
async function compare(a,b,read){
  const names=[...new Set([...a.keys(),...b.keys()])].sort((x,y)=>x.localeCompare(y,"en",{numeric:true}));
  if(!names.length) throw Error("No gl_*.ppm captures found");
  const rows=[];
  for(const name of names){
    let result;
    if(!a.has(name)||!b.has(name)) result={status:a.has(name)?"Missing candidate":"Missing baseline"};
    else {
      try {result=difference(frame(await read(a.get(name))),frame(await read(b.get(name))));}
      catch(e){throw Error(name+": "+e.message);}
    }
    rows.push({name,...result});
  }
  return rows;
}
function report(rows){
  const changed=rows.filter(r=>r.status!=="Identical");
  return ["# Pokke Lab comparison","",rows.length+" filenames compared; "+changed.length+" differences.",
    "Matched by filename. Capture timing and simulation alignment require independent confirmation.","",
    ...changed.map(r=>"- "+JSON.stringify(r)),""].join("\n");
}
if(typeof module!=="undefined") module.exports={frame,difference,compare,report};
