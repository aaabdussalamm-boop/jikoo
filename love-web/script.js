function jawabYa() {
  document.getElementById("result").innerHTML = "Samaa aku juga suka sama kamu ❤️🥰";
}

function kabur() {
  const noBtn = document.getElementById("noBtn");
  const container = document.querySelector(".container");

  const maxX = container.clientWidth - noBtn.clientWidth;
  const maxY = container.clientHeight - noBtn.clientHeight;

  const randomX = Math.floor(Math.random() * maxX);
  const randomY = Math.floor(Math.random() * maxY);

  noBtn.style.position = "absolute";
  noBtn.style.left = randomX + "px";
  noBtn.style.top = randomY + "px";
}
