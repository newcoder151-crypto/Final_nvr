module.exports = {
  openapi: '3.0.3',
  info: { title: 'Railway NVR API', version: '2.0.0',
    description: 'mNVR PostgreSQL backend for Railway Vision AI frontend. No Supabase.' },
  servers: [
    { url: 'http://localhost:3001', description: 'Development' },
    { url: 'https://your-server.com', description: 'Production' },
  ],
  components: {
    securitySchemes: { bearerAuth: { type: 'http', scheme: 'bearer', bearerFormat: 'JWT' } },
  },
  security: [{ bearerAuth: [] }],
  paths: {
    '/api/health': { get: { summary: 'Health check', security: [], tags: ['System'], responses: { 200: { description: 'OK' } } } },
    '/api/auth/login': { post: { summary: 'Login', security: [], tags: ['Auth'],
      requestBody: { required: true, content: { 'application/json': { schema: { type: 'object', required: ['username','password'], properties: { username: { type: 'string' }, password: { type: 'string' } } } } } },
      responses: { 200: { description: 'JWT token + user' }, 401: { description: 'Bad credentials' } } } },
    '/api/auth/register': { post: { summary: 'Register', security: [], tags: ['Auth'],
      requestBody: { required: true, content: { 'application/json': { schema: { type: 'object', properties: { username:{type:'string'}, password:{type:'string'}, full_name:{type:'string'}, email:{type:'string'} } } } } },
      responses: { 201: { description: 'Created' } } } },
    '/api/auth/me': { get: { summary: 'Current user', tags: ['Auth'], responses: { 200: { description: 'User profile' } } } },
    '/api/cameras': {
      get: { summary: 'List cameras with health', tags: ['Cameras'], parameters: [
          { name:'status',in:'query',schema:{type:'string'} }, { name:'limit',in:'query',schema:{type:'integer',default:100} } ],
        responses: { 200: { description: 'Cameras + health' } } },
      post: { summary: 'Add camera (ADMIN)', tags: ['Cameras'],
        requestBody: { required: true, content: { 'application/json': { schema: { type:'object', required:['camera_name'], properties: {
          camera_name:{type:'string'}, camera_type:{type:'string',enum:['INTERIOR','EXTERIOR','DOOR','DRIVER_CAB']},
          ip_address:{type:'string'}, rtsp_url:{type:'string'}, location_description:{type:'string'} } } } } },
        responses: { 201: { description: 'Camera created' } } } },
    '/api/cameras/{id}': {
      get: { summary: 'Get camera', tags: ['Cameras'], parameters: [{name:'id',in:'path',required:true,schema:{type:'integer'}}], responses: { 200:{description:'Camera'} } },
      put: { summary: 'Update camera', tags: ['Cameras'], parameters: [{name:'id',in:'path',required:true,schema:{type:'integer'}}], responses: { 200:{description:'Updated'} } },
      delete: { summary: 'Delete camera (ADMIN)', tags: ['Cameras'], parameters: [{name:'id',in:'path',required:true,schema:{type:'integer'}}], responses: { 200:{description:'Deleted'} } } },
    '/api/cameras/{id}/health': {
      get: { summary: 'Camera health history', tags: ['Cameras'], parameters: [{name:'id',in:'path',required:true,schema:{type:'integer'}}], responses: { 200:{description:'Health metrics'} } },
      post: { summary: 'Push health metric', tags: ['Cameras'], parameters: [{name:'id',in:'path',required:true,schema:{type:'integer'}}], responses: { 200:{description:'Recorded'} } } },
    '/api/recordings': {
      get: { summary: 'List recordings', tags: ['Recordings'], parameters: [
          { name:'camera_id',in:'query',schema:{type:'integer'} }, { name:'start_date',in:'query',schema:{type:'string',format:'date-time'} },
          { name:'end_date',in:'query',schema:{type:'string',format:'date-time'} }, { name:'limit',in:'query',schema:{type:'integer',default:50} } ],
        responses: { 200:{description:'Recording list'} } },
      post: { summary: 'Create recording entry (mNVR process)', tags: ['Recordings'], responses: { 201:{description:'Created'} } } },
    '/api/recordings/{id}': {
      get: { summary: 'Get recording', tags: ['Recordings'], parameters: [{name:'id',in:'path',required:true,schema:{type:'integer'}}], responses: { 200:{description:'Recording'} } },
      delete: { summary: 'Delete recording', tags: ['Recordings'], parameters: [{name:'id',in:'path',required:true,schema:{type:'integer'}}], responses: { 200:{description:'Deleted'} } } },
    '/api/streaming/recordings/{id}/stream': { get: { summary: 'Stream MP4 (byte-range)', tags: ['Streaming'],
      parameters: [{name:'id',in:'path',required:true,schema:{type:'integer'}},{name:'token',in:'query',schema:{type:'string'}}],
      responses: { 206:{description:'Partial content'} } } },
    '/api/streaming/hls/{cameraId}/stream.m3u8': { get: { summary: 'HLS live stream', tags: ['Streaming'],
      parameters: [{name:'cameraId',in:'path',required:true,schema:{type:'integer'}},{name:'token',in:'query',schema:{type:'string'}}],
      responses: { 200:{description:'M3U8 playlist'} } } },
    '/api/events': {
      get: { summary: 'List events/alerts', tags: ['Events'], parameters: [
          { name:'camera_id',in:'query',schema:{type:'integer'} }, { name:'severity',in:'query',schema:{type:'string'} },
          { name:'acknowledged',in:'query',schema:{type:'boolean'} }, { name:'limit',in:'query',schema:{type:'integer',default:100} } ],
        responses: { 200:{description:'Events'} } },
      post: { summary: 'Create event', tags: ['Events'], responses: { 201:{description:'Created'} } } },
    '/api/events/{id}/acknowledge': { put: { summary: 'Acknowledge event', tags: ['Events'],
      parameters: [{name:'id',in:'path',required:true,schema:{type:'integer'}}], responses: { 200:{description:'Acknowledged'} } } },
    '/api/events/all/acknowledge': { put: { summary: 'Acknowledge ALL events', tags: ['Events'], responses: { 200:{description:'Acknowledged'} } } },
    '/api/users': {
      get: { summary: 'List users (ADMIN)', tags: ['Users'], responses: { 200:{description:'Users'} } },
      post: { summary: 'Create user (ADMIN)', tags: ['Users'], responses: { 201:{description:'Created'} } } },
    '/api/config/status': { get: { summary: 'System status', tags: ['Config'], responses: { 200:{description:'System metrics'} } } },
    '/api/config/dashboard': { get: { summary: 'Dashboard stats (cameras + recent alerts)', tags: ['Config'], responses: { 200:{description:'Dashboard data'} } } },
    '/api/config': { get: { summary: 'All config keys', tags: ['Config'], responses: { 200:{description:'Config'} } } },
    '/api/ai/detect': { post: { summary: 'Generic YOLO detection', tags: ['AI'],
      requestBody: { required:true, content: { 'multipart/form-data': { schema: { type:'object', properties: { image:{type:'string',format:'binary'}, conf:{type:'number'}, model:{type:'string'} } } } } },
      responses: { 200:{description:'Detections'}, 502:{description:'Sidecar down'} } } },
    '/api/ai/people-count': { post: { summary: 'People count + crowd density', tags: ['AI'],
      requestBody: { required:true, content: { 'multipart/form-data': { schema: { type:'object', properties: { image:{type:'string',format:'binary'}, camera_id:{type:'integer'}, conf:{type:'number'} } } } } },
      responses: { 200:{description:'People count + density'} } } },
    '/api/ai/intrusion': { post: { summary: 'Zone intrusion detection', tags: ['AI'],
      requestBody: { required:true, content: { 'multipart/form-data': { schema: { type:'object', properties: { image:{type:'string',format:'binary'}, zone:{type:'string'}, camera_id:{type:'integer'} } } } } },
      responses: { 200:{description:'Intrusion result'} } } },
    '/api/ai/analytics': { get: { summary: 'AI event analytics from DB', tags: ['AI'],
      parameters: [{name:'since',in:'query',schema:{type:'string',format:'date-time'}}], responses: { 200:{description:'Analytics'} } } },
    '/api/ai/health': { get: { summary: 'YOLO sidecar health', tags: ['AI'], responses: { 200:{description:'Up'}, 503:{description:'Down'} } } },
  },
};
