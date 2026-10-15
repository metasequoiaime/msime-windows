// Shared runtime; generated schema.js must be loaded first in classic pages.
(function (root) {
  'use strict';
  const schema = typeof module !== 'undefined' && module.exports
    ? require('./schema.js') : root.MsimeMessageSchema;
  const object = value => value !== null && typeof value === 'object' && !Array.isArray(value);
  function matches(value, rule) {
    if (rule.anyOf) return rule.anyOf.some(branch => matches(value, branch));
    if (rule.type === 'array') {
      if (!Array.isArray(value) || !value.every(item => matches(item, rule.items))) return false;
    } else if (rule.type === 'object') {
      if (!object(value)) return false;
      const properties = rule.properties || {};
      if ((rule.required || []).some(key => !Object.hasOwn(value, key))) return false;
      for (const key of Object.keys(value)) {
        if (Object.hasOwn(properties, key)) {
          if (!matches(value[key], properties[key])) return false;
        } else if (rule.additionalProperties === false) return false;
      }
    } else if (rule.type === 'integer') {
      if (!Number.isSafeInteger(value)) return false;
    } else if (rule.type === 'number') {
      if (typeof value !== 'number' || !Number.isFinite(value)) return false;
    } else if (rule.type === 'null') {
      if (value !== null) return false;
    } else if (rule.type && typeof value !== rule.type) return false;
    if (rule.enum && !rule.enum.includes(value)) return false;
    if (rule.minimum !== undefined && value < rule.minimum) return false;
    if (rule.maximum !== undefined && value > rule.maximum) return false;
    return true;
  }
  function validate(message, direction, surface) {
    if (!object(message) || typeof message.type !== 'string') return false;
    if (message.protocolVersion !== undefined && message.protocolVersion !== schema.version) return false;
    const rules = schema[direction];
    if (!rules || !Object.hasOwn(rules, message.type)) return false;
    const spec = rules[message.type];
    if (spec.surfaces && !spec.surfaces.includes(surface)) return false;
    if (spec.data && (!Object.hasOwn(message, 'data') || !matches(message.data, spec.data))) return false;
    if (!spec.data && Object.hasOwn(message, 'data')) return false;
    return matches(message, { type: 'object', properties: {
      type: { type: 'string' }, protocolVersion: { type: 'integer' },
      ...(spec.data ? {data: spec.data} : {}), ...(spec.properties || {})
    }, required: spec.required || [], additionalProperties: spec.additionalProperties === true });
  }
  function serializeClientMessage(message, surface) {
    const normalized = JSON.parse(JSON.stringify({...message, protocolVersion: schema.version}));
    if (!validate(normalized, 'client', surface)) throw new TypeError('Invalid MSIME ' + surface + ' message');
    return JSON.stringify(normalized);
  }
  const api = { validate, serializeClientMessage, version: schema.version };
  root.MsimeProtocol = api;
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
})(globalThis);
