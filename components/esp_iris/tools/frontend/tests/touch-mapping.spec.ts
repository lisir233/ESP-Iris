import { expect, test } from "@playwright/test";
import { mapPointerToContainedMedia } from "../src/Workspace";

const surface = { left: 248, top: 302, width: 869, height: 530 };
const screen = { width: 480, height: 480 };

test("maps pointer coordinates to the contained screen instead of its host", () => {
  expect(mapPointerToContainedMedia(442.5, 567, surface, screen)).toEqual({ x: 0, y: 5000 });
  expect(mapPointerToContainedMedia(682.5, 567, surface, screen)).toEqual({ x: 5000, y: 5000 });
  expect(mapPointerToContainedMedia(922.5, 567, surface, screen)).toEqual({ x: 10000, y: 5000 });
});

test("rejects letterbox starts and clamps captured drags to screen edges", () => {
  expect(mapPointerToContainedMedia(300, 567, surface, screen)).toBeNull();
  expect(mapPointerToContainedMedia(300, 567, surface, screen, true)).toEqual({ x: 0, y: 5000 });
});
