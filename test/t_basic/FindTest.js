/*
 Copyright (c) 2012, Oracle and/or its affiliates. All rights
 reserved.
 
 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License
 as published by the Free Software Foundation; version 2 of
 the License.
 
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 02110-1301  USA
 */

/***** Find with domain object and primitive primary key ***/
t1 = new harness.ConcurrentTest("testFindDomainObjectPrimitive");
t1.run = function() {
  var testCase = this;
  // use the domain object and primitive to find an instance
  var from = global.t_basic;
  var key = 1;
  fail_openSession(testCase, function(session) {
    // key and testCase are passed to fail_verify_t_basic as extra parameters
    session.find(from, key, fail_verify_t_basic, key, testCase);
  });
};

/***** Find with domain object and javascript primary key literal ***/
t2 = new harness.ConcurrentTest("testFindDomainObjectLiteral");
t2.run = function() {
  var testCase = this;
  // use the domain object and literal to find an instance
  var from = global.t_basic;
  var key = {'id' : 2};
  fail_openSession(testCase, function(session) {
    // key and testCase are passed to fail_verify_t_basic as extra parameters
    session.find(from, key, fail_verify_t_basic, 2, testCase);
  });
};

/***** Find with domain object and javascript primary key object ***/
t3 = new harness.ConcurrentTest("testFindDomainObjectObject");
t3.run = function() {
  var testCase = this;
  // use the domain object and key object to find an instance
  var from = global.t_basic;
  var key = new t_basic_key(3);
  fail_openSession(testCase, function(session) {
    // key and testCase are passed to fail_verify_t_basic as extra parameters
    session.find(from, key, fail_verify_t_basic, 3, testCase);
  });
};

/***** Find with table name and primitive primary key ***/
t4 = new harness.ConcurrentTest("testFindTableNamePrimitive");
t4.run = function() {
  var testCase = this;
  var from = 't_basic';
  var key = 4;
  fail_openSession(testCase, function(session) {
    // key and testCase are passed to fail_verify_t_basic as extra parameters
    session.find(from, key, fail_verify_t_basic, 4, testCase);
  });
};

/***** Find with table name and javascript primary key literal ***/
t5 = new harness.ConcurrentTest("testFindTableNameLiteral");
t5.run = function() {
  var testCase = this;
  // use table name and literal to find an instance
  var from = 't_basic';
  var key = {'id' : 5};
  fail_openSession(testCase, function(session) {
    // key and testCase are passed to fail_verify_t_basic as extra parameters
    session.find(from, key, fail_verify_t_basic, 5, testCase);
  });
};

/***** Find with table name and javascript primary key object ***/
t6 = new harness.ConcurrentTest("testFindTableNameObject");
t6.run = function() {
  var testCase = this;
  var from = 't_basic';
  var key = new t_basic_key(6);
  fail_openSession(testCase, function(session) {
    // key and testCase are passed to fail_verify_t_basic as extra parameters
    session.find(from, key, fail_verify_t_basic, 6, testCase);
  });
};

/***** Find with domain object and javascript unique key literal ***/
t7 = new harness.ConcurrentTest("testFindDomainObjectUniqueKeyLiteral");
t7.run = function() {
  var testCase = this;
  // use the domain object and literal to find an instance
  var from = global.t_basic;
  var key = {'magic' : 7};
  fail_openSession(testCase, function(session) {
    // key and testCase are passed to fail_verify_t_basic as extra parameters
    session.find(from, key, fail_verify_t_basic, 7, testCase);
  });
};

/***** Find with domain object and javascript ordered key literal ***/
t8 = new harness.ConcurrentTest("testFindDomainObjectOrderedIndexLiteral");
t8.run = function() {
  var testCase = this;
  // use the domain object and literal to find an instance
  var from = global.t_basic;
  var key = {'age' : 8};
  fail_openSession(testCase, function(session) {
    // key and testCase are passed to fail_verify_t_basic as extra parameters
    session.find(from, key, fail_verify_t_basic, 8, testCase);
  });
};

/***** Find with table name and javascript unique key literal ***/
t9 = new harness.ConcurrentTest("testFindTableNameUniqueKeyLiteral");
t9.run = function() {
  var testCase = this;
  // use the table name and literal to find an instance
  var from = 't_basic';
  var key = {'magic' : 9};
  fail_openSession(testCase, function(session) {
    // key and testCase are passed to fail_verify_t_basic as extra parameters
    session.find(from, key, fail_verify_t_basic, 9, testCase);
  });
};

/***** Find with table name and javascript ordered key literal ***/
t0 = new harness.ConcurrentTest("testFindTableNameOrderedIndexLiteral");
t0.run = function() {
  var testCase = this;
  // use the table name and literal to find an instance
  var from = 't_basic';
  var key = {'age': 0};
  fail_openSession(testCase, function(session) {
    // key and testCase are passed to fail_verify_t_basic as extra parameters
    session.find(from, key, fail_verify_t_basic, 0, testCase);
  });
};

module.exports.tests = [t1, t2, t3, t4, t5, t6, t7, t8, t9, t0];