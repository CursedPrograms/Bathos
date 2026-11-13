// Bathos's page: the feature cards and the year
(function () {
  'use strict';
  const features = [
    ['Monocular depth', 'Depth Anything V2 through ONNX Runtime, on the CPU: about half a second for a 2 MP photo. No Python, no GPU needed.'],
    ['Proper faces', 'Every face is fitted with a 468-point 3D face mesh, so noses, eye sockets, cheeks and chins have real depth, feathered into the hair.'],
    ['Refined edges', 'A guided filter snaps the depth to the picture\'s own outlines: hair, branches and skylines without blocky steps.'],
    ['The window', 'Drop a picture and drag the wipe between it and its depth. Detail, colour ramp, far-is-bright and contrast, then save as JPEG or PNG.'],
    ['Batches', 'Drop a pile of pictures: each gets its depth map beside it. Or run bathos-cli on as many as you like.'],
    ['In Chimera', 'Filter > Bathos > Depth Map puts the depth of the picture, or a layer, in as a new layer, masked to the selection.']
  ];
  const grid = document.getElementById('features');
  if (grid) {
    for (const [title, text] of features) {
      const card = document.createElement('article');
      const h = document.createElement('h2');
      h.textContent = title;
      const p = document.createElement('p');
      p.textContent = text;
      card.append(h, p);
      grid.appendChild(card);
    }
  }
  const year = document.getElementById('year');
  if (year) year.textContent = String(Math.max(2026, new Date().getFullYear()));
})();
