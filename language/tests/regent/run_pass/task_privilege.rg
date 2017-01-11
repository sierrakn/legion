-- Copyright 2017 Stanford University
--
-- Licensed under the Apache License, Version 2.0 (the "License");
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     http://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

import "regent"

task f(r : region(int), x : ptr(int, r)) : int
where reads(r) do
  return @x
end

task g(r : region(int), x : ptr(int, r))
where reads(r), writes(r) do
  @x = 5
end

task t() : int
  var r = region(ispace(ptr, 5), int)
  var x = new(ptr(int, r))
  g(r, x)
  return f(r, x)
end

task main()
  regentlib.assert(t() == 5, "test failed")
end
regentlib.start(main)
