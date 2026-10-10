import { deflateSync } from 'node:zlib';
import { createHash } from 'node:crypto';
import { mkdir, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

export const question = 'Count the red circles and blue squares in the image. Report red_circles, blue_squares, and weighted = 2 * red_circles + blue_squares as three integer fields in JSON.';
export const width = 640;
export const height = 480;
const scenes = [
  { name: 'scene-a', shapes: [['red', 130, 120], ['blue', 320, 120], ['red', 510, 120], ['blue', 225, 330], ['red', 415, 330]] },
  { name: 'scene-b', shapes: [['red', 130, 120], ['red', 320, 120], ['blue', 510, 120], ['red', 130, 330], ['red', 320, 330], ['red', 510, 330]] },
];
const table = Array.from({ length: 256 }, (_, n) => {
  for (let bit = 0; bit < 8; ++bit) n = (n & 1) ? 0xedb88320 ^ (n >>> 1) : n >>> 1;
  return n >>> 0;
});
function crc32(bytes) {
  let crc = 0xffffffff;
  for (const byte of bytes) crc = table[(crc ^ byte) & 255] ^ (crc >>> 8);
  return (crc ^ 0xffffffff) >>> 0;
}
function chunk(type, bytes) {
  const tag = Buffer.from(type, 'ascii');
  const size = Buffer.alloc(4); size.writeUInt32BE(bytes.length);
  const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(Buffer.concat([tag, bytes])));
  return Buffer.concat([size, tag, bytes, crc]);
}
function render(shapes) {
  const pixels = Buffer.alloc(height * (1 + width * 3), 255);
  for (let y = 0; y < height; ++y) pixels[y * (1 + width * 3)] = 0;
  for (const [color, cx, cy] of shapes) {
    for (let y = cy - 42; y <= cy + 42; ++y) {
      for (let x = cx - 42; x <= cx + 42; ++x) {
        const inside = color === 'red' ? (x - cx) ** 2 + (y - cy) ** 2 <= 42 ** 2 : Math.abs(x - cx) <= 40 && Math.abs(y - cy) <= 40;
        if (!inside) continue;
        const at = y * (1 + width * 3) + 1 + x * 3;
        pixels[at] = color === 'red' ? 230 : 20;
        pixels[at + 1] = color === 'red' ? 25 : 65;
        pixels[at + 2] = color === 'red' ? 35 : 230;
      }
    }
  }
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(width); ihdr.writeUInt32BE(height, 4);
  ihdr[8] = 8; ihdr[9] = 2;
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr), chunk('IDAT', deflateSync(pixels, { level: 9 })), chunk('IEND', Buffer.alloc(0))]);
}
export function fixtureScenes() {
  return scenes.map(scene => {
    const red_circles = scene.shapes.filter(([color]) => color === 'red').length;
    const blue_squares = scene.shapes.filter(([color]) => color === 'blue').length;
    const png = render(scene.shapes);
    return { name: scene.name, width, height, mime: 'image/png', png,
      sha256: createHash('sha256').update(png).digest('hex'),
      expected: { red_circles, blue_squares, weighted: 2 * red_circles + blue_squares } };
  });
}
export function cppHeader() {
  const rows = fixtureScenes().map(scene => {
    const encoded = scene.png.toString('base64');
    const lines = encoded.match(/.{1,96}/g).map(line => `      "${line}"`).join('\n');
    return `inline const Scene& ${scene.name === 'scene-a' ? 'scene_a' : 'scene_b'}() {\n  static const Scene value{${scene.expected.red_circles}, ${scene.expected.blue_squares}, ${scene.expected.weighted},\n    std::make_shared<const std::string>(\n${lines})};\n  return value;\n}`;
  });
  return `#pragma once\n// Generated from tools/vision_fixture.mjs geometry; no production encoder goldens.\n#include "core/media.h"\n#include <memory>\n#include <string>\n#include <string_view>\nnamespace vision_test {\ninline constexpr std::string_view question = ${JSON.stringify(question)};\ninline constexpr std::string_view secret = "VISION_SYNTHETIC_KEY";\ninline constexpr std::string_view private_signature = "VISION_PRIVATE_SIGNATURE";\nstruct Scene {\n  unsigned red, blue, weighted;\n  std::shared_ptr<const std::string> base64;\n  sp::Media image() const { return sp::Media::image("image/png", base64); }\n  std::string answer() const {\n    return "{\\\"red_circles\\\":" + std::to_string(red) + ",\\\"blue_squares\\\":" + std::to_string(blue) + ",\\\"weighted\\\":" + std::to_string(weighted) + '}';\n  }\n};\n${rows.join('\n')}\n} // namespace vision_test\n`;
}
if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  if (process.argv.length !== 3) throw new Error('usage: node tools/vision_fixture.mjs BUILD_DIRECTORY');
  const directory = resolve(process.argv[2]); await mkdir(directory, { recursive: true });
  const manifest = { version: 1, synthetic: true, question, scenes: [] };
  for (const scene of fixtureScenes()) {
    await writeFile(resolve(directory, `${scene.name}.png`), scene.png);
    manifest.scenes.push({ name: scene.name, filename: `${scene.name}.png`, mime: scene.mime,
      width, height, bytes: scene.png.length, sha256: scene.sha256, expected: scene.expected,
      base64: scene.png.toString('base64') });
  }
  await writeFile(resolve(directory, 'vision-manifest.json'), JSON.stringify(manifest, null, 2) + '\n');
  process.stdout.write(JSON.stringify({ directory, images: manifest.scenes.map(({ filename, sha256, expected }) => ({ filename, sha256, expected })) }) + '\n');
}
