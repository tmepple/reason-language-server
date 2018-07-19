
open Typedtree;
open SharedTypes;
open Infix;

let rec dig = (typ) =>
  switch typ.Types.desc {
  | Types.Tlink(inner) => dig(inner)
  | Types.Tsubst(inner) => dig(inner)
  | _ => typ
  };

let rec relative = (ident, path) =>
  switch (ident, path) {
  | (Longident.Lident(name), Path.Pdot(path, pname, _)) when pname == name => Some(path)
  | (Longident.Ldot(ident, name), Path.Pdot(path, pname, _)) when pname == name => relative(ident, path)
  /* | (Ldot(Lident("*predef*" | "exn"), _), Pident(_)) => None */
  | _ => None
  };

let addOpen = (extra, path, loc, extent, ident) => {
  let op = {path, loc, used: [], extent, ident};
  Hashtbl.add(extra.opens, loc, op);
};

let findClosestMatchingOpen = (opens, path, ident, loc) => {
  let%opt openNeedle = relative(ident, path);
  /* Log.log("<> Finding an open " ++ Path.name(path));
  Log.log("Ident " ++ String.concat(".", Longident.flatten(ident)));
  Log.log("Relative thing " ++ Path.name(openNeedle)); */

  let matching = Hashtbl.fold((l, op, res) => {
    if (Utils.locWithinLoc(loc, op.extent) && Path.same(op.path, openNeedle)) {
      [op, ...res]
    } else {
      res
    }
  }, opens, []) |. Belt.List.sort((a, b) => {
    open Location;
    a.loc.loc_start.pos_cnum - b.loc.loc_start.pos_cnum
  });

  switch matching {
    | [] => None
    | [first, ..._] => Some(first)
  }
};

module F = (Collector: {
  let extra: extra;
  let file: file;
  let scopeExtent: ref(list(Location.t));
}) => {
  let extra = Collector.extra;

  let maybeAddUse = (path, ident, loc, tip) => {
    let%opt_consume tracker = findClosestMatchingOpen(extra.opens, path, ident, loc);
    let%opt_consume relpath = Query.makeRelativePath(tracker.path, path);

    /* switch (Query.makePath(path)) {
    | `Stamp(name) =>
      /* This shouldn't happen */
      ()
    | `Path((_stamp, _name, ourPath)) => */
      tracker.used = [(relpath, tip, loc), ...tracker.used];
    /* } */
  };


  let addLocation = (loc, ident) => extra.locations = [(loc, ident), ...extra.locations];
  let addReference = (stamp, loc) => Hashtbl.replace(extra.internalReferences, stamp, [loc, ...Hashtbl.mem(extra.internalReferences, stamp) ? Hashtbl.find(extra.internalReferences, stamp) : []]);
  let addExternalReference = (moduleName, path, tip, loc) => Hashtbl.replace(extra.externalReferences, moduleName, [(path, tip, loc), ...Hashtbl.mem(extra.externalReferences, moduleName) ? Hashtbl.find(extra.externalReferences, moduleName) : []]);
  let env = {Query.file: Collector.file, exported: Collector.file.contents.exported};

  let getTypeAtPath = path => {
    switch (Query.fromCompilerPath(~env, path)) {
    | `Global(moduleName, path) => `Global(moduleName, path)
    | `Not_found => `Not_found
    | `Exported(env, name) => {
        let res = {
          let%opt stamp = Query.hashFind(env.exported.types, name);
          let%opt_wrap declaredType = Query.hashFind(env.file.stamps.types, stamp);
          `Local(declaredType)
        };
        res |? `Not_found
    }
    | `Stamp(stamp) => {
      let res = {
        let%opt_wrap declaredType = Query.hashFind(env.file.stamps.types, stamp);
        `Local(declaredType)
      };
      res |? `Not_found
    }
    }
  };

  let addForPath = (path, lident, loc, typ, tip) => {
    maybeAddUse(path, lident, loc, tip);
    let identName = Longident.last(lident);
    let identLoc = Utils.endOfLocation(loc, String.length(identName));
    let locType = switch (Query.fromCompilerPath(~env, path)) {
      | `Stamp(stamp) => {
        addReference(stamp, identLoc);
        Loc.LocalReference(stamp, tip);
      }
      | `Not_found => Loc.NotFound
      | `Global(moduleName, path) => {
        addExternalReference(moduleName, path, tip, identLoc);
        Loc.GlobalReference(moduleName, path, tip)
      }
      | `Exported(env, name) => {
        let res = {
          let%opt_wrap stamp = Query.hashFind(env.exported.values, name);
          addReference(stamp, identLoc);
          Loc.LocalReference(stamp, tip)
        };
        res |? Loc.NotFound
      }
    };
    addLocation(loc, Loc.Typed(typ, locType));
  };

  let addForField = (recordType, item, {Asttypes.txt, loc}) => {
    switch (dig(recordType).desc) {
      | Tconstr(path, _args, _memo) => {
        let t = getTypeAtPath(path);
        let {Types.lbl_loc, lbl_res} = item;
        let name = Longident.last(txt);

        let (name, typeLident) = Definition.handleConstructor(path, txt);
        maybeAddUse(path, typeLident, loc, Constructor(name));

        let nameLoc = Utils.endOfLocation(loc, String.length(name));
        let locType = switch (t) {
          | `Local({stamp, contents: {kind: Record(attributes)}}) => {
            {
              let%opt_wrap {stamp: astamp} = Belt.List.getBy(attributes, a => a.name.txt == name);
              addReference(astamp, nameLoc);
              Loc.LocalReference(stamp, Attribute(name));
            } |? Loc.NotFound
          }
          | `Global(moduleName, path) =>
            addExternalReference(moduleName, path, Attribute(name), nameLoc);
            Loc.GlobalReference(moduleName, path, Attribute(name))
          | _ => Loc.NotFound
        };
        addLocation(nameLoc, Loc.Typed(lbl_res, locType))
      }
      | _ => ()
    }
  };

  let addForRecord = (recordType, items) => {
    switch (dig(recordType).desc) {
      | Tconstr(path, _args, _memo) => {
        let t = getTypeAtPath(path);
        items |> List.iter((({Asttypes.txt, loc}, {Types.lbl_loc, lbl_res}, _)) => {
          let name = Longident.last(txt);
          let nameLoc = Utils.endOfLocation(loc, String.length(name));
          let locType = switch (t) {
            | `Local({stamp, contents: {kind: Record(attributes)}}) => {
              {
                let%opt_wrap {stamp: astamp} = Belt.List.getBy(attributes, a => a.name.txt == name);
                addReference(astamp, nameLoc);
                Loc.LocalReference(stamp, Attribute(name));
              } |? Loc.NotFound
            }
            | `Global(moduleName, path) =>
              addExternalReference(moduleName, path, Attribute(name), nameLoc);
              Loc.GlobalReference(moduleName, path, Attribute(name))
            | _ => Loc.NotFound
          };
          addLocation(nameLoc, Loc.Typed(lbl_res, locType))
        })
      }
      | _ => ()
    }
  };

  let addForConstructor = (constructorType, {Asttypes.txt, loc}, {Types.cstr_name, cstr_loc}) => {
    switch (dig(constructorType).desc) {
      | Tconstr(path, _args, _memo) => {
        let name = Longident.last(txt);
        let nameLoc = Utils.endOfLocation(loc, String.length(name));
        let locType = switch (getTypeAtPath(path)) {
          | `Local({stamp, contents: {kind: Variant(constructos)}}) => {
            {
              let%opt_wrap {stamp: cstamp} = Belt.List.getBy(constructos, a => a.name.txt == cstr_name);
              addReference(cstamp, nameLoc);
              Loc.LocalReference(stamp, Constructor(name))
            } |? Loc.NotFound
          }
          | `Global(moduleName, path) =>
            addExternalReference(moduleName, path, Constructor(name), nameLoc);
            Loc.GlobalReference(moduleName, path, Constructor(name))
          | _ => Loc.NotFound
        };
        addLocation(nameLoc, Loc.Typed(constructorType, locType));
      }
      | _ => ()
    }
  };

  let currentScopeExtent = () => List.hd(Collector.scopeExtent^);
  let addScopeExtent = loc => Collector.scopeExtent := [loc, ...Collector.scopeExtent^];
  let popScopeExtent = () => Collector.scopeExtent := List.tl(Collector.scopeExtent^);

  open Typedtree;
  include TypedtreeIter.DefaultIteratorArgument;
  let enter_structure_item = item => switch (item.str_desc) {
  | Tstr_attribute(({Asttypes.txt: "ocaml.explanation", loc}, PStr([{pstr_desc: Pstr_eval({pexp_desc: Pexp_constant(Const_string(doc, _))}, _)}]))) => {
    addLocation(loc, Loc.Explanation(doc))
  }
  | Tstr_open({open_path, open_txt: {txt, loc} as l}) => {
    /* Log.log("Have an open here"); */
    maybeAddUse(open_path, txt, loc, Module);
    let tracker = {
      path: open_path,
      loc,
      ident: l,
      used: [],
      extent: {
        loc_ghost: true,
        loc_start: loc.loc_end,
        loc_end: currentScopeExtent().loc_end,
      }
    };
    Hashtbl.replace(Collector.extra.opens, loc, tracker);
  }
  | _ => ()
  };

  let enter_structure = ({str_items}) => {
    let first = List.hd(str_items);
    let last = List.nth(str_items, List.length(str_items) - 1);

    let extent = {
      Location.loc_ghost: true,
      loc_start: first.str_loc.loc_start,
      loc_end: last.str_loc.loc_end,
    };

    addScopeExtent(extent);
  };

  let leave_structure = str => {
    popScopeExtent();
  };

  let enter_signature_item = item => switch (item.sig_desc) {
  | Tsig_value({val_id: {stamp}, val_loc, val_name: name, val_desc, val_attributes}) => {
    if (!Hashtbl.mem(Collector.file.stamps.values, stamp)) {
      let declared = ProcessCmt.newDeclared(
        ~name,
        ~stamp,
        ~extent=val_loc,
        ~modulePath=NotVisible,
        ~processDoc=x => x,
        ~contents={Value.typ: val_desc.ctyp_type, recursive: false},
        false,
        val_attributes
      );
      Hashtbl.add(Collector.file.stamps.values, stamp, declared);
      addReference(stamp, name.loc);
      addLocation(name.loc, Loc.Typed(val_desc.ctyp_type, Loc.Definition(stamp, Value)));
    }
  }
  | _ => ()
  };

  let enter_core_type = ({ctyp_loc, ctyp_type, ctyp_desc}) => {
    switch (ctyp_desc) {
      | Ttyp_constr(path, {txt, loc}, args) => {
        addForPath(path, txt, loc, ctyp_type, Type)
      }
      | _ => ()
    }
  };

  let enter_pattern = ({pat_desc, pat_loc, pat_type, pat_attributes}) => {
    switch (pat_desc) {
      | Tpat_record(items, _) => {
        addForRecord(pat_type, items);
      }
      | Tpat_construct(lident, constructor, _) => {
        addForConstructor(pat_type, lident, constructor)
      }
      | Tpat_var({stamp}, name) => {
        if (!Hashtbl.mem(Collector.file.stamps.values, stamp)) {
          let declared = ProcessCmt.newDeclared(
            ~name,
            ~stamp,
            ~modulePath=NotVisible,
            ~extent=pat_loc,
            ~processDoc=x => x,
            ~contents={Value.typ: pat_type, recursive: false},
            false,
            pat_attributes
          );
          Hashtbl.add(Collector.file.stamps.values, stamp, declared);
          addReference(stamp, name.loc);
          addLocation(name.loc, Loc.Typed(pat_type, Loc.Definition(stamp, Value)));
        }
      }
      | _ => ()
    }
  };

  let enter_expression = expression => {
    expression.exp_extra |. Belt.List.forEach(((e, eloc, _)) => switch e {
      | Texp_open(_, path, ident, _) => {
        extra.opens |. Hashtbl.add(eloc, {
          path,
          ident,
          loc: eloc,
          extent: expression.exp_loc,
          used: [],
        })
      }
      | _ => ()
    });
    switch (expression.exp_desc) {
    | Texp_ident(path, {txt, loc}, {val_type}) => {
      addForPath(path, txt, loc, val_type, Value);
    }
    | Texp_record(items, _) => {
      addForRecord(expression.exp_type, items);
    }
    | Texp_construct(lident, constructor, _args) => {
      addForConstructor(expression.exp_type, lident, constructor);
    }
    | Texp_field(inner, lident, label_description) => {
      addForField(inner.exp_type, label_description, lident)
    }
    | _ => ()
    }
  };
};

/* let noType = {Types.id: 0, level: 0, desc: Tnil}; */

let forItems = (~file, items) => {
  let extra = initExtra();
  let addLocation = (loc, ident) => extra.locations = [(loc, ident), ...extra.locations];
  let addReference = (stamp, loc) => Hashtbl.replace(extra.internalReferences, stamp, [loc, ...Hashtbl.mem(extra.internalReferences, stamp) ? Hashtbl.find(extra.internalReferences, stamp) : []]);
  file.stamps.modules |> Hashtbl.iter((stamp, d) => {
    addLocation(d.name.loc, Loc.Module(Loc.Definition(stamp, Module)));
    addReference(stamp, d.name.loc);
  });
  file.stamps.values |> Hashtbl.iter((stamp, d) => {
    addLocation(d.name.loc, Loc.Typed(d.contents.Value.typ, Loc.Definition(stamp, Value)));
    addReference(stamp, d.name.loc);
  });
  file.stamps.types |> Hashtbl.iter((stamp, d) => {
    addLocation(d.name.loc, Loc.TypeDefinition(d.name.txt, d.contents.Type.typ, stamp));
    addReference(stamp, d.name.loc);
    switch (d.contents.Type.kind) {
      | Record(labels) => labels |> List.iter(({Type.Attribute.stamp, name, typ, typLoc}) => {
        addReference(stamp, name.loc);
        addLocation(name.loc, Loc.Typed(typ, Loc.Definition(d.stamp, Attribute(name.txt))))
      });
      | Variant(constructos) => constructos |> List.iter(({Type.Constructor.stamp, name}) => {
        addReference(stamp, name.loc);
        let t = {Types.id: 0, level: 0, desc: Tconstr(Path.Pident({Ident.stamp, name: d.name.txt, flags: 0}), [], ref(Types.Mnil))};
        addLocation(name.loc, Loc.Typed(t, Loc.Definition(d.stamp, Constructor(name.txt))))
      });
      | _ => ()
    };
  });

  let first = List.hd(items);
  let last = List.nth(items, List.length(items) - 1);

  let extent = {
    Location.loc_ghost: true,
    loc_start: first.str_loc.loc_start,
    loc_end: last.str_loc.loc_end,
  };

  let module Iter = TypedtreeIter.MakeIterator(F({
    let scopeExtent = ref([extent]);
    let extra = extra;
    let file = file;
  }));

  /* Iter.iter_structure(items); */
  List.iter(Iter.iter_structure_item, items);
  extra
};

open Result;
let forCmt = (~file, {cmt_modname, cmt_annots}: Cmt_format.cmt_infos) => switch cmt_annots {
| Partial_implementation(parts) => {
  let items = parts |. Array.to_list |. Belt.List.keepMap(p => switch p {
    | Partial_structure(str) => Some(str.str_items)
    | Partial_structure_item(str) => Some([str])
    | _ => None
  }) |> List.concat;
  Ok(forItems(~file, items))
}
| Implementation(structure) => {
  Ok(forItems(~file, structure.str_items))
}
| _ => Error("Invalid cmt file")
};
