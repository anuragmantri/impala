# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

import json
import requests
import pytest

from tests.common.custom_cluster_test_suite import CustomClusterTestSuite


class TestWebPage(CustomClusterTestSuite):
  @pytest.mark.execute_serially
  @CustomClusterTestSuite.with_args(
      impalad_args="--enable_extended_memory_metrics=true"
  )
  def test_varz_hidden_variables(self):
    """Tests that modified hidden variables show up in /varz"""
    response = requests.get("http://localhost:25000/varz?json")
    assert response.status_code == requests.codes.ok
    varz_json = json.loads(response.text)
    flag = [e for e in varz_json["flags"]
            if e["name"] == "enable_extended_memory_metrics"]
    assert len(flag) == 1
    assert flag[0]["default"] == "false"
    assert flag[0]["current"] == "true"
    assert flag[0]["experimental"]

  @pytest.mark.execute_serially
  @CustomClusterTestSuite.with_args(
      impalad_args="--use_local_catalog=true",
      catalogd_args="--catalog_topic_mode=minimal"
  )
  def test_observability(self):
    """
    Tests observabilty on the web UI by scraping the webpage
    and search for patterns
    """

    self.test_root_webpage()

  def test_root_webpage(self):
    """ Test observability on root webpage """
    response = requests.get("http://localhost:25000/")
    assert response.status_code == requests.codes.ok
    # Test 'Local Catalog' Banner appears on root page
    assert '(Local Catalog Mode)' in response.text
    