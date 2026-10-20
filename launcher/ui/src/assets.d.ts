// Images are inlined into the single-file UI build.
declare module '*.png' {
  const url: string;
  export default url;
}
