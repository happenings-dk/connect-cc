cmake_minimum_required(VERSION 3.25)
file(READ "${MANIFEST}" manifest)
string(JSON manifest SET "${manifest}" dependencies 0
  "{\"name\":\"connectcpp\",\"features\":[\"lite\"]}")
file(WRITE "${MANIFEST}" "${manifest}\n")
