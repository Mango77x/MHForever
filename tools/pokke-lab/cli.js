const fs=require("node:fs/promises");
const path=require("node:path");
const {compare,report}=require("./compare");
async function captures(dir){
  const items=await fs.readdir(dir,{withFileTypes:true});
  return new Map(items.filter(f=>f.isFile()&&/^gl_.*\.ppm$/i.test(f.name)).map(f=>[f.name,path.join(dir,f.name)]));
}
async function main(){
  const args=process.argv.slice(2);
  if(args.length!==2) throw Error("Usage: node cli.js BASELINE CANDIDATE");
  const rows=await compare(await captures(args[0]),await captures(args[1]),p=>fs.readFile(p));
  process.stdout.write(report(rows));
  process.exitCode=Number(rows.some(r=>r.status!=="Identical"));
}
main().catch(e=>{console.error(e.message);process.exitCode=2;});
