// Protobuf helper for EXAMVAN — dual JSON/Protobuf for 2c/8GB optimal
// proto/examvan.proto -> static/js/protobuf-helper.js (fetch + encode)
// When PROTOBUF_MANDATORY=1, all POST/PUT must use application/x-protobuf
//
// P21-T3: branch CDN dihapus. CDN unpkg memakai SRI placeholder
// (sha384-PLACEHOLDER…) sehingga browser selalu menolak script-nya, dan
// bundle lokal /static/js/protobuf.min.js yang dijanjikan pesan error tidak
// pernah ada di repo — artinya loadProtobuf() tidak punya jalur sukses.
// Client web saat ini tidak memakai protobuf client-side (PROTOBUF_MANDATORY
// hanya mengenai client Android); helper menolak eksplisit daripada mencoba
// memuat script pihak ketiga tanpa hash valid.

let _protobufRoot = null;
async function loadProtobuf() {
  if (_protobufRoot) return _protobufRoot;
  if (typeof protobuf === 'undefined') {
    throw new Error('protobufjs tidak tersedia di halaman ini — encode client-side dinonaktifkan');
  }
  _protobufRoot = await protobuf.load('/proto/examvan.proto');
  return _protobufRoot;
}

async function encodeCreateExam(name, filePath, sizeBytes, customToken, pdfData) {
  const root = await loadProtobuf();
  const Type = root.lookupType('examvan.v1.CreateExamRequest');
  const msg = Type.create({ name, filePath, sizeBytes, customToken, pdfData });
  return Type.encode(msg).finish();
}

function isProtobufMandatory() {
  // Set by server via meta tag or window.__PROTOBUF_MANDATORY
  const meta = document.querySelector('meta[name="protobuf-mandatory"]');
  return (meta && meta.content === '1') || window.__PROTOBUF_MANDATORY === true;
}

async function apiFetchProtobuf(url, options = {}) {
  const mandatory = isProtobufMandatory();
  const method = (options.method || 'GET').toUpperCase();
  if (['POST','PUT','PATCH'].includes(method) && mandatory) {
    options.headers = options.headers || {};
    if (!options.headers['Content-Type']) options.headers['Content-Type'] = 'application/x-protobuf';
    options.headers['Accept'] = 'application/x-protobuf';
  }
  // For dual-support, try protobuf first, fallback to JSON on 415
  let res = await apiFetch(url, options);
  if (res.status === 415 && mandatory) {
    console.warn('Protobuf required, retry with protobuf');
  }
  return res;
}
