// FIXME: ICs must work in parallel.
// FIXME: Must be able to call native intrinsics in parallel with a JSContext,
//        or have native intrinsics provide both a sequential and a parallel
//        version.
// TODO: Multi-dimensional.
// TODO: Use let over var when Ion compiles let.
// TODO: Private names.
// XXX: Experiment with cloning the whole op.
// XXX: Hide buffer and other fields?

function ComputeTileBounds(len, id, n) {
  var slice = (len / n) | 0;
  var start = slice * id;
  var end = id === n - 1 ? len : slice * (id + 1);
  return [start, end];
}

function ComputeProducts(shape) {
  // Compute the partial products in reverse order.
  // e.g., if the shape is [A,B,C,D], then the
  // array |products| will be [1,D,CD,BCD].
  var product = 1;
  var products = [];
  var sdimensionality = shape.length;
  for (var i = sdimensionality - 1; i >= 0; i--) {
    products.push(product);
    product *= shape[i];
  }
  return products;
}

function ComputeIndices(shape, index1d) {
  // Given a shape and some index |index1d|, computes and returns an
  // array containing the N-dimensional index that maps to |index1d|.

  var products = ComputeProducts(shape);
  var l = shape.length;

  var result = [];
  for (var i = 0; i < l; i++) {
    // Obtain product of all higher dimensions.
    // So if i == 0 and shape is [A,B,C,D], yields BCD.
    var stride = products[l - i - 1];

    // Compute how many steps of width stride we could take.
    var index = (index1d / stride) | 0;
    result[i] = index;

    // Adjust remaining indices for smaller dimensions.
    index1d -= (index * stride);
  }

  return result;
}

function StepIndices(shape, indices) {
  var i = shape.length - 1;
  while (i >= 0) {
    var indexi = indices[i] + 1;
    if (indexi < shape[i]) {
      indices[i] = indexi;
      return;
    }
    indices[i] = 0;
    i--;
  }
}

// Constructor
//
// We split the 3 construction cases so that we don't case on arguments, which
// deoptimizes.

function ParallelArrayConstruct0() {
  this.buffer = %_SetNonBuiltinCallerInitObjectType([]);
  this.offset = 0;
  this.shape = [0];
  this.get = ParallelArrayGet1;
}

function ParallelArrayConstruct1(buffer) {
  var buffer = %ToObject(buffer);
  var length = buffer.length >>> 0;
  if (length !== buffer.length)
    %ThrowError(JSMSG_PAR_ARRAY_BAD_ARG, "");

  var buffer1 = %_SetNonBuiltinCallerInitObjectType([]);
  for (var i = 0; i < length; i++) {
    buffer1[i] = buffer[i];
  }

  this.buffer = buffer1;
  this.offset = 0;
  this.shape = [length];
  this.get = ParallelArrayGet1;
}

function ParallelArrayConstruct2(shape, f) {
  if (typeof shape === 'number') {
    return ParallelArrayBuild(this, [shape], f);
  } else {
    return ParallelArrayBuild(this, shape, f);
  }
}

function ParallelArrayConstruct3(shape, buffer, offset) {
  this.shape = shape;
  this.buffer = buffer;
  this.offset = offset;
  this.get = ParallelArrayGetN;

  if (shape.length == 1) {
    this.get = ParallelArrayGet1;
  } else if (shape.length == 2) {
    this.get = ParallelArrayGet2;
  } else if (shape.length == 3) {
    this.get = ParallelArrayGet3;
  }
}

function ParallelArrayBuild(self, shape, f) {
  self.shape = shape;
  self.offset = 0;

  if (shape.length === 1) {
    var length = shape[0];
    var buffer = %ParallelBuildArray(length, fill1, f);
    if (!buffer) {
      buffer = %_SetNonBuiltinCallerInitObjectType([]);
      buffer.length = length;
      fill1(buffer, 0, 1, f);
    }

    self.get = ParallelArrayGet1;
    self.buffer = buffer;
  } else if (shape.length === 2) {
    var length = shape[0] * shape[1];
    var buffer = %ParallelBuildArray(length, fill2, shape[1], f);
    if (!buffer) {
      buffer = %_SetNonBuiltinCallerInitObjectType([]);
      buffer.length = length;
      fill2(buffer, 0, 1, shape[1], f);
    }

    self.get = ParallelArrayGet2;
    self.buffer = buffer;
  } else if (shape.length == 3) {
    var length = shape[0] * shape[1] * shape[2];
    var buffer = %ParallelBuildArray(length, fill3, shape[1], shape[2], f);
    if (!buffer) {
      buffer = %_SetNonBuiltinCallerInitObjectType([]);
      buffer.length = length;
      fill3(buffer, 0, 1, shape[1], shape[2], f);
    }

    self.get = ParallelArrayGet3;
    self.buffer = buffer;
  } else {
    var length = 1;
    for (var i = 0; i < shape.length; i++) {
      length *= shape[i];
    }

    var buffer = %ParallelBuildArray(length, fillN, shape, f);
    if (!buffer) {
      buffer = %_SetNonBuiltinCallerInitObjectType([]);
      buffer.length = length;
      fillN(buffer, 0, 1, shape, f);
    }

    self.get = ParallelArrayGetN;
    self.buffer = buffer;
  }

  function fill1(result, id, n, f) {
    var [start, end] = ComputeTileBounds(result.length, id, n);
    for (var i = start; i < end; i++) {
      result[i] = f(i);
    }
  }

  function fill2(result, id, n, yw, f) {
    var [start, end] = ComputeTileBounds(result.length, id, n);
    var x = (start / yw) | 0;
    var y = start - x*yw;
    for (var i = start; i < end; i++) {
      result[i] = f(x, y);
      if (++y == yw) {
        y = 0;
        ++x;
      }
    }
  }

  function fill3(result, id, n, yw, zw, f) {
    var [start, end] = ComputeTileBounds(result.length, id, n);
    var x = (start / (yw*zw)) | 0;
    var r = start - x*yw*zw;
    var y = (r / zw) | 0;
    var z = r - y*zw;
    for (var i = start; i < end; i++) {
      result[i] = f(x, y, z);
      if (++z == zw) {
        z = 0;
        if (++y == yw) {
          y = 0;
          ++x;
        }
      }
    }
  }

  function fillN(result, id, n, shape, f) {
    // NB: In fact this will not currently be parallelized due to the
    // use of `f.apply()`.  But it's written as if it could be.  A guy
    // can dream, can't he?
    var [start, end] = ComputeTileBounds(result.length, id, n);
    var indices = ComputeIndices(shape, start);
    for (var i = start; i < end; i++) {
      result[i] = f.apply(null, indices);
      StepIndices(shape, indices);
    }
  }
}

function ParallelArrayMap(f) {
  function fill(result, id, n, f, self) {
    var [start, end] = ComputeTileBounds(self.shape[0], id, n);
    for (var i = start; i < end; i++) {
      result[i] = f(self.get(i));
    }
  }

  var length = this.shape[0];
  var buffer = %ParallelBuildArray(length, fill, f, this);
  if (!buffer) {
    buffer = %_SetNonBuiltinCallerInitObjectType([]);
    fill(buffer, 0, 1, f, this);
  }
  return new global.ParallelArray([buffer.length], buffer, 0);
}

function ParallelArrayReduce(f) {
  var source = this.buffer;
  var length = source.length;

  if (length === 0)
    %ThrowError(JSMSG_PAR_ARRAY_REDUCE_EMPTY);

  function reduce(source, start, end, f) {
    // The accumulator: the objet petit a.
    //
    // "A VM's accumulator register is Objet petit a: the unattainable object
    // of desire that sets in motion the symbolic movement of interpretation."
    //     -- PLT Zizek
    var a = source[start];
    for (var i = start+1; i < end; i++)
      a = f(a, source[i]);
    return a;
  }

  function fill(result, id, n, source, f) {
    // Mildly awkward: in the real parallel phase, there will be
    // precisely one entry in result per worker.  But in the warmup
    // phase, that is not so!  Therefore, we store the reduced version
    // into |id % result.length| so as to ensure that in the warmup
    // phase |id| never exceeds the length of result.
    var [start, end] = ComputeTileBounds(source.length, id, n);
    //result[id % result.length] = reduce(source, start, end, f);

    var a = source[start];
    for (var i = start+1; i < end; i++)
      a = f(a, source[i]);
    result[id % result.length] = a;
  }

  var threads = %_GetThreadPoolInfo().numThreads;
  var subreductions = %ParallelBuildArray(threads, fill, source, f);
  if (subreductions) {
    return reduce(subreductions, 0, subreductions.length, f);
  } else {
    return reduce(source, 0, length, f);
  }
}

function ParallelArrayScan(f) {
  var length = this.shape[0];

  if (length === 0)
    %ThrowError(JSMSG_PAR_ARRAY_REDUCE_EMPTY);

  var buffer = %_SetNonBuiltinCallerInitObjectType([]);
  var a = this.get(0);
  for (var i = 1; i < length; i++) {
    a = f(a, this.get(i));
    result[i] = a;
  }

  return new global.ParallelArray(buffer);
}

function ParallelArrayScatter(targets, zero, f, length) {
  // TODO: N-dimensional
  // TODO: Parallelize. %ThrowError or any calling of intrinsics isn't safe.
  function fill(result, id, n, targets, zero, f, source) {
    var length = result.length;

    // Initialize a conflict array and initialize the result to the zero value.
    var conflict = [];
    var [start, end] = ComputeTileBounds(length, id, n);
    for (var i = start; i < end; i++) {
      result[i] = zero;
      conflict[i] = false;
    }

    var limit = length < targets.length ? length : targets.length;
    var [start, end] = ComputeTileBounds(limit, id, n);

    for (var i = start; i < end; i++) {
      var t = targets[i];

      if (t >>> 0 !== t)
        %ThrowError(JSMSG_PAR_ARRAY_BAD_ARG, ".prototype.scatter");

      if (t >= length)
        %ThrowError(JSMSG_PAR_ARRAY_SCATTER_BOUNDS);

      if (conflict[t]) {
        if (!f)
          %ThrowError(JSMSG_PAR_ARRAY_SCATTER_CONFLICT);
        result[t] = f(source[i], result[t]);
      } else {
        result[t] = source[i];
        conflict[t] = true;
      }
    }
  }

  var source = this.buffer;

  if (targets.length >>> 0 !== targets.length)
    %ThrowError(JSMSG_BAD_ARRAY_LENGTH, "");
  if (length && length >>> 0 !== length)
    %ThrowError(JSMSG_BAD_ARRAY_LENGTH, "");

  var buffer = %_SetNonBuiltinCallerInitObjectType([]);
  buffer.length = length || source.length;
  fill(buffer, 0, 1, targets, zero, f, source);

  return new global.ParallelArray(buffer);
}

function ParallelArrayFilter(filters) {
  // TODO: Parallelize.
  var length = this.shape[0];

  if (filters.length >>> 0 !== length)
    %ThrowError(JSMSG_BAD_ARRAY_LENGTH, "");

  var buffer = %_SetNonBuiltinCallerInitObjectType([]);
  for (var i = 0, pos = 0; i < length; i++) {
    if (filters[i])
      buffer[pos++] = this.get(i);
  }
  return new global.ParallelArray(buffer);
}

function ParallelArrayPartition(amount) {
  if (amount >>> 0 !== amount)
    %ThrowError(JSMSG_BAD_ARRAY_LENGTH, ""); // XXX
  
  var length = this.shape[0];
  var partitions = (length / amount) | 0;
  
  if (partitions * amount !== length)
    %ThrowError(JSMSG_BAD_ARRAY_LENGTH, ""); // XXX

  var shape = [amount, partitions];
  for (var i = 1; i < this.shape.length; i++)
    shape.push(this.shape[i]);
  return new global.ParallelArray(shape, this.buffer, this.offset);
}

//
// Accessors and utilities.
//

function ParallelArrayGet1(i) {
  var udef; // For some reason `undefined` doesn't work
  if (i === udef) {
    return this;
  } else {
    return this.buffer[this.offset + i];
  }
}

function ParallelArrayGet2(x, y) {
  var udef; // For some reason `undefined` doesn't work
  var yw = this.shape[1];
  if (x === udef) {
    return this;
  } else if (y === udef) {
    return new global.ParallelArray([yw], this.buffer, this.offset + x*yw);
  } else {
    var offset = y + x*yw;
    return this.buffer[this.offset + offset];
  }
}

function ParallelArrayGet3(x, y, z) {
  var udef; // For some reason `undefined` doesn't work
  var yw = this.shape[1];
  var zw = this.shape[2];
  if (x === udef) {
    return this;
  } else if (y === udef) {
    return new global.ParallelArray([yw, zw], this.buffer, this.offset + x*yw*zw);
  } else if (z === udef) {
    return new global.ParallelArray([zw], this.buffer, this.offset + y*zw + x*yw*zw);
  } else {
    var offset = z + y*zw + x*yw*zw;
    return this.buffer[this.offset + offset];
  }
}

function ParallelArrayGetN(...coords) {
  if (coords.length == 0)
    return this;

  var products = ComputeProducts(this.shape);

  // Compute the offset of the given coordinates.  Each index is
  // multipled by its corresponding entry in the |products|
  // array, counting in reverse.  So if |coords| is [a,b,c,d],
  // then you get |a*BCD + b*CD + c*D + d|.
  var offset = this.offset;
  var sdimensionality = this.shape.length;
  var cdimensionality = coords.length;
  for (var i = 0; i < cdimensionality; i++) {
    offset += coords[i] * products[sdimensionality - i - 1];
  }

  if (cdimensionality < sdimensionality) {
    var shape = this.shape.slice(cdimensionality);
    return new global.ParallelArray(shape, this.buffer, offset);
  } else {
    return this.buffer[offset];
  }
}

function ParallelArrayLength() {
  return this.buffer.length;
}

function ParallelArrayToString() {
  return this.buffer.toString();
}

// Unit Test Functions
//
// function CheckIndices(shape, index1d) {
//   let idx = ComputeIndices(shape, index1d);
// 
//   let c = 0;
//   for (var i = 0; i < shape.length; i++) {
//     var stride = 1;
//     for (var j = i + 1; j < shape.length; j++) {
//       stride *= shape[j];
//     }
//     c += idx[i] * stride;
//   }
//   
//   assertEq(index1d, c);
// }
// 
// for (var q = 0; q < 2*4*6*8; q++) {
//   CheckIndices([2,4,6,8], q);
// }
