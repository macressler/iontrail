﻿/**
 * Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/
 */
"use strict";

var self = this;

var testGenerator = testSteps();

function testSteps()
{
  const dbName = self.window ?
                 window.location.pathname :
                 "test_transaction_abort_hang";
  const objStoreName = "foo";
  const transactionCount = 30;

  let completedTransactionCount = 0;
  let caughtError = false;

  let abortedTransactionIndex = Math.floor(transactionCount / 2);
  if (abortedTransactionIndex % 2 == 0) {
    abortedTransactionIndex++;
  }

  let request = indexedDB.open(name, 1);
  request.onerror = errorHandler;
  request.onupgradeneeded = grabEventAndContinueHandler;
  let event = yield;

  request.result.createObjectStore(objStoreName, { autoIncrement: true });

  request.onupgradeneeded = null;
  request.onsuccess = grabEventAndContinueHandler;
  event = yield;

  let db = event.target.result;

  for (let i = 0; i < transactionCount; i++) {
    const readonly = i % 2 == 0;
    const mode = readonly ? "readonly" : "readwrite";

    let transaction = db.transaction(objStoreName, mode);

    if (i == transactionCount - 1) {
      // Last one, finish the test.
      transaction.oncomplete = grabEventAndContinueHandler;
    } else if (i == abortedTransactionIndex - 1) {
      transaction.oncomplete = function(event) {
        ok(true, "Completed transaction " + ++completedTransactionCount +
           " (We may hang after this!)");
      };
    } else if (i == abortedTransactionIndex) {
      // Special transaction that we abort outside the normal event flow.
      transaction.onerror = function(event) {
        ok(true, "Aborted transaction " + ++completedTransactionCount +
           " (We didn't hang!)");
        is(event.target.error.name, "AbortError",
           "AbortError set as the error on the request");
        is(event.target.transaction.error, null,
           "No error set on the transaction");
        ok(!caughtError, "Haven't seen the error event yet");
        caughtError = true;
        event.preventDefault();
      };
      // This has to happen after the we return to the event loop but before the
      // transaction starts running.
      setTimeout(function() { transaction.abort(); }, 0);
    } else {
      transaction.oncomplete = function(event) {
        ok(true, "Completed transaction " + ++completedTransactionCount);
      };
    }

    if (readonly) {
      transaction.objectStore(objStoreName).get(0);
    } else {
      try { transaction.objectStore(objStoreName).add({}); } catch(e) { }
    }
  }
  ok(true, "Created all transactions");

  event = yield;

  ok(true, "Completed transaction " + ++completedTransactionCount);
  ok(caughtError, "Caught the error event when we aborted the transaction");

  finishTest();
  yield;
}
