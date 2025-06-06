#!/usr/bin/env ruby

# Copyright 2015 gRPC authors.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

require_relative './end2end_common'

def main
  # point to a non-existant server on IPv6 discard prefix
  ch = GRPC::Core::Channel.new("[0100::]:80", {},
                               :this_channel_is_insecure)
  thr = Thread.new do
    loop do
      state = ch.connectivity_state
      ch.watch_connectivity_state(state, Time.now + 360)
    end
  end
  # sleep to allow time to get into the middle of a
  # connectivity state watch operation
  sleep 0.1
  thr.kill
  thr.join
end

main
