//
// Copyright (C) 2012 Colin Walters <walters@verbum.org>
//
// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2 of the License, or (at your option) any later version.
//
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the
// Free Software Foundation, Inc., 59 Temple Place - Suite 330,
// Boston, MA 02111-1307, USA.

const Lang = imports.lang;

const JsonUtil = imports.jsonutil;
const Params = imports.params;

function _componentDict(snapshot) {
    let r = {};
    let components = snapshot['components'];
    for (let i = 0; i < components.length; i++) {
	let component = components[i];
	let name = component['name'];
	if (r[name])
            throw new Error("Duplicate component name " + name);
        r[name] = component;
    }

    let patches = snapshot['patches'];
    if (patches['name'])
        r[patches['name']] = patches;

    let base = snapshot['base'];
    r[base['name']] = base;
    return r;
}

function snapshotDiff(a, b) {
    let a_components = _componentDict(a);
    let b_components = _componentDict(b);

    let added = [];
    let modified = [];
    let removed = [];

    for (let name in a_components) {
        let c_a = a_components[name];
        let c_b = b_components[name];
        if (c_b == undefined) {
            removed.push(name);
	} else if (c_a['revision'] != c_b['revision']) {
            modified.push(name);
	}
    }
    for (let name in b_components) {
        if (a_components[name] == undefined) {
            added.push(name);
	}
    }
    return [added, modified, removed];
}

function fromFile(path, cancellable, args) {
    let data = JsonUtil.loadJson(path, cancellable);
    return new Snapshot(data, path, args);
}

const Snapshot = new Lang.Class({
    Name: 'Snapshot',
    
    _init: function(data, path, params) {
	params = Params.parse(params, { prepareResolve: false });
	this.data = data;
	this.path = path;
	if (params.prepareResolve) {
	    data['patches'] = this._resolveComponent(data, data['patches']);
	    data['base'] = this._resolveComponent(data, data['base']);
	    for (let i = 0; i < data['components'].length; i++) {
		let component = this._resolveComponent(data, data['components'][i]);
		data['components'][i] = component;
	    }
	}
	this._componentDict = _componentDict(data);
	this._componentNames = [];
	for (let k in this._componentDict)
	    this._componentNames.push(k);
    },

    _resolveComponent: function(manifest, componentMeta) {
        if (!componentMeta)
            return {};

	let result = {};
	Lang.copyProperties(componentMeta, result);
	let origSrc = componentMeta['src'];
	let name = componentMeta['name'];

	if (origSrc.indexOf('tarball:') == 0) {
	    if (!name)
		throw new Error("Component src " + origSrc + " has no name attribute");
	    if (!componentMeta['checksum'])
		throw new Error("Component src " + origSrc + " has no checksum attribute");
	    return result;
	}

	let didExpand = false;
	let vcsConfig = manifest['vcsconfig'];
	for (let vcsprefix in vcsConfig) {
	    let expansion = vcsConfig[vcsprefix];
            let prefix = vcsprefix + ':';
            if (origSrc.indexOf(prefix) == 0) {
		result['src'] = expansion + origSrc.substr(prefix.length);
		didExpand = true;
		break;
	    }
	}

	let src, idx;
	if (name == undefined) {
            if (didExpand) {
		src = origSrc;
		idx = src.lastIndexOf(':');
		name = src.substr(idx+1);
            } else {
		src = result['src'];
		idx = src.lastIndexOf('/');
		name = src.substr(idx+1);
	    }
	    let i = name.lastIndexOf('.git');
            if (i != -1 && i == name.length - 4) {
		name = name.substr(0, name.length - 4);
	    }
            name = name.replace(/\//g, '-');
            result['name'] = name;
	}

	let branchOrTag = result['branch'] || result['tag'];
	if (!branchOrTag) {
            result['branch'] = 'master';
	}

	return result;
    },

    _expandComponent: function(component) {
	let r = {};
	Lang.copyProperties(component, r);
	let patchMeta = this.data['patches'];
	if (patchMeta) {
	    let componentPatchFiles = component['patches'] || [];
	    if (componentPatchFiles.length > 0) {
		let patches = {};
		Lang.copyProperties(patchMeta, patches);
		patches['files'] = componentPatchFiles;
		r['patches'] = patches;
	    }
	}
	let configOpts = (this.data['config-opts'] || []).concat();
	configOpts.push.apply(configOpts, component['config-opts'] || []);
	r['config-opts'] = configOpts;
	return r;
    },

    getAllComponentNames: function() {
	return this._componentNames;
    },

    getComponentMap: function() {
	return this._componentDict;
    },

    getComponent: function(name, allowNone) {
	let r = this._componentDict[name] || null;
	if (!r && !allowNone)
	    throw new Error("No component " + name + " in snapshot");
	return r;
    },

    getMatchingSrc: function(src, allowNone) {
	let result = [];
	for (let i = 0; i < this._componentNames.length; i++) {
	    let name = this._componentNames[i];
	    let component = this.getComponent(name, false);
	    if (component['src'] == src)
		result.push(component);
	}
	return result;
    },

    getExpanded: function(name) {
	return this._expandComponent(this.getComponent(name));
    }
});
