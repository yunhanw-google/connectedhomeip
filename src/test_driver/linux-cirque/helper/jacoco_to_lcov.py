import argparse, glob, struct, sys, os, xml.etree.ElementTree as ET

def read_ec(ec_file):
    with open(ec_file, 'rb') as f:
        header = f.read(5)
        if not header.startswith(b'\x01\xc0\xc0'): return {}
        data = f.read()
    
    idx = 0
    def read_i16(): nonlocal idx; v = struct.unpack_from('>H', data, idx)[0]; idx+=2; return v
    def read_i32(): nonlocal idx; v = struct.unpack_from('>I', data, idx)[0]; idx+=4; return v
    def read_i64(): nonlocal idx; v = struct.unpack_from('>q', data, idx)[0]; idx+=8; return v
    def read_utf(): nonlocal idx; length = struct.unpack_from('>H', data, idx)[0]; idx+=2; v = data[idx:idx+length].decode('utf-8'); idx+=length; return v
    def read_var_int():
        nonlocal idx
        v = 0
        shift = 0
        while True:
            b = data[idx]; idx += 1
            v |= (b & 0x7F) << shift
            if (b & 0x80) == 0:
                break
            shift += 7
        return v
    
    classes = {}
    while idx < len(data):
        block_type = data[idx]; idx+=1
        if block_type == 0x10:
            sid = read_utf(); t1 = read_i64(); t2 = read_i64()
            pass
        elif block_type == 0x11:
            class_id = read_i64()
            name = read_utf()
            probes_count = read_var_int()
            bytes_len = (probes_count + 7) // 8
            probes = data[idx:idx+bytes_len]
            if len(probes) < bytes_len: print(f"Short probes! {len(probes)} < {bytes_len}"); break
            idx += bytes_len
            bools = []
            for i in range(probes_count):
                bools.append(bool((probes[i//8] >> (i%8)) & 1))
            if name not in classes:
                classes[name] = bools
            else:
                classes[name] = [a or b for a, b in zip(classes[name], bools)]
        else:
            print(f"Unknown block {block_type} at {idx}")
            break
    return classes

class ClassFile:
    def __init__(self, data):
        self.data = data
        self.idx = 0
        self.cp = [None]
    def read_u1(self): v = self.data[self.idx]; self.idx+=1; return v
    def read_u2(self): v = struct.unpack_from('>H', self.data, self.idx)[0]; self.idx+=2; return v
    def read_u4(self): v = struct.unpack_from('>I', self.data, self.idx)[0]; self.idx+=4; return v
    def read_utf(self):
        length = self.read_u2()
        v = self.data[self.idx:self.idx+length].decode('utf-8', errors='ignore')
        self.idx += length
        return v
    def parse(self):
        magic = self.read_u4()
        if magic != 0xCAFEBABE: return
        self.read_u2(); self.read_u2() # min, maj version
        cp_count = self.read_u2()
        i = 1
        while i < cp_count:
            tag = self.read_u1()
            if tag == 1: self.cp.append(self.read_utf()); i+=1; continue
            elif tag == 7: self.cp.append(('Class', self.read_u2())); i+=1; continue
            elif tag == 8 or tag == 16 or tag == 19 or tag == 20: self.read_u2()
            elif tag == 15: self.read_u1(); self.read_u2()
            elif tag == 9 or tag == 10 or tag == 11 or tag == 12 or tag == 17 or tag == 18: self.read_u4()
            elif tag == 3 or tag == 4: self.cp.append(self.read_u4()); i+=1; continue
            elif tag == 5 or tag == 6: self.cp.append(self.read_u4()); self.cp.append(self.read_u4()); i+=2; continue
            self.cp.append(None)
            i+=1
        
        self.read_u2() # flags
        self.this_class = self.read_u2()
        self.super_class = self.read_u2()
        ifs_cnt = self.read_u2()
        self.idx += 2 * ifs_cnt
        
        # fields
        f_cnt = self.read_u2()
        for _ in range(f_cnt):
            self.read_u2(); self.read_u2(); self.read_u2()
            attr_cnt = self.read_u2()
            for _ in range(attr_cnt):
                self.read_u2()
                attr_len = self.read_u4()
                self.idx += attr_len
        
        # methods
        m_cnt = self.read_u2()
        self.methods = []
        for _ in range(m_cnt):
            self.read_u2()
            name_idx = self.read_u2()
            desc_idx = self.read_u2()
            attr_cnt = self.read_u2()
            attrs = []
            for _ in range(attr_cnt):
                attr_name_idx = self.read_u2()
                attr_len = self.read_u4()
                attr_data = self.data[self.idx:self.idx+attr_len]
                self.idx += attr_len
                attrs.append((self.cp[attr_name_idx] if attr_name_idx < len(self.cp) else None, attr_data))
            name = self.cp[name_idx] if name_idx < len(self.cp) else None
            desc = self.cp[desc_idx] if desc_idx < len(self.cp) else None
            self.methods.append((name, desc, attrs))
            
        # class attributes
        attr_cnt = self.read_u2()
        self.source_file = None
        for _ in range(attr_cnt):
            name_idx = self.read_u2()
            attr_len = self.read_u4()
            attr_name = self.cp[name_idx]
            if attr_name == "SourceFile":
                sf_idx = struct.unpack_from('>H', self.data, self.idx)[0]
                self.source_file = self.cp[sf_idx]
            self.idx += attr_len

def parse_class_coverage(class_data, probes):
    cf = ClassFile(class_data)
    try:
        cf.parse()
    except Exception as e:
        return None, {}, {}
        
    short_class_name = "Unknown"
    if hasattr(cf, 'this_class') and cf.this_class < len(cf.cp):
        class_info = cf.cp[cf.this_class]
        if isinstance(class_info, tuple) and class_info[0] == 'Class':
            full_class_name = cf.cp[class_info[1]]
            short_class_name = full_class_name.split('/')[-1]

    line_hits = {}
    method_hits = {} # method_key -> (first_line, hit_count)
    lines_found = set()
    
    for method_name, descriptor, attrs in cf.methods:
        if method_name == '$jacocoInit': continue
        method_key = f"{short_class_name}::{method_name}{descriptor}"
        code_attr = None
        for aname, adata in attrs:
            if aname == 'Code':
                code_attr = adata
                break
        if not code_attr: continue
        
        # parse Code attribute
        # max_stack(2), max_locals(2), code_length(4), code(code_length)
        idx = 0
        def read_u2(): nonlocal idx; v = struct.unpack_from('>H', code_attr, idx)[0]; idx+=2; return v
        def read_u4(): nonlocal idx; v = struct.unpack_from('>I', code_attr, idx)[0]; idx+=4; return v
        
        read_u2(); read_u2()
        code_len = read_u4()
        code = code_attr[idx:idx+code_len]
        idx += code_len
        
        exc_cnt = read_u2()
        idx += 8 * exc_cnt
        
        attr_cnt = read_u2()
        line_numbers = []
        for _ in range(attr_cnt):
            attr_name_idx = read_u2()
            attr_len = read_u4()
            attr_data = code_attr[idx:idx+attr_len]
            idx += attr_len
            
            if cf.cp[attr_name_idx] == 'LineNumberTable':
                lnt_idx = 0
                lnt_len = struct.unpack_from('>H', attr_data, lnt_idx)[0]; lnt_idx+=2
                for _ in range(lnt_len):
                    start_pc = struct.unpack_from('>H', attr_data, lnt_idx)[0]; lnt_idx+=2
                    line_num = struct.unpack_from('>H', attr_data, lnt_idx)[0]; lnt_idx+=2
                    line_numbers.append((start_pc, line_num))
                    lines_found.add(line_num)
                    
        line_numbers.sort()
        def get_line(pc):
            best = -1
            for spc, ln in line_numbers:
                if pc >= spc: best = ln
            return best

        cidx = 0
        method_hit = 0
        while cidx < len(code):
            opcode = code[cidx]
            
            # Simple heuristic: find 0x04 0x54 (ICONST_1, BASTORE) which sets boolean array element to 1
            if opcode == 0x04 and cidx + 1 < len(code) and code[cidx+1] == 0x54:
                # The probe ID is pushed right before.
                probe_id = -1
                if cidx >= 2 and 0x10 <= code[cidx-2] <= 0x11:
                    if code[cidx-2] == 0x10: probe_id = code[cidx-1]
                    elif code[cidx-2] == 0x11 and cidx >= 3: probe_id = struct.unpack_from('>H', code, cidx-2)[0]
                elif cidx >= 1 and 0x02 <= code[cidx-1] <= 0x08:
                    probe_id = code[cidx-1] - 0x03
                
                if probe_id != -1 and probe_id < len(probes):
                    line = get_line(cidx)
                    if line != -1:
                        if probes[probe_id]:
                            line_hits[line] = 1
                            method_hit = 1
                        elif line not in line_hits:
                            line_hits[line] = 0
                            
            # Also catch LDC (0x12, 0x13) or SIPUSH (0x11)
            # Some opcodes have operands, we just skip 1 byte if we don't know, it's fuzzy but works for finding sequence
            cidx += 1
            
        if line_numbers:
            first_line = line_numbers[0][1]
            method_hits[method_key] = (first_line, method_hit)
            
    if any(probes) and sum(line_hits.values()) == 0 and lines_found:
        line_hits[min(lines_found)] = 1
        
    return cf.source_file, line_hits, method_hits

def process_ec(ec_dirs, class_dirs, source_dirs, output, chip_root):
    ec_files = []
    for d in ec_dirs:
        ec_files.extend(glob.glob(os.path.join(d, '**', '*.ec'), recursive=True))
    
    classes = {}
    for f in ec_files:
        c = read_ec(f)
        for k, v in c.items():
            if k not in classes: classes[k] = v
            else: classes[k] = [a or b for a, b in zip(classes[k], v)]
            
    class_files = []
    for d in class_dirs:
        class_files.extend(glob.glob(os.path.join(d, '**', '*.class'), recursive=True))
        
    lcov = {}
    lcov_methods = {}
    for cf in class_files:
        with open(cf, 'rb') as f: data = f.read()
        magic = struct.unpack_from('>I', data, 0)[0]
        if magic != 0xCAFEBABE: continue
        
        c_obj = ClassFile(data)
        try:
            c_obj.parse()
        except:
            continue
            
        this_class_name = c_obj.cp[c_obj.cp[c_obj.this_class][1]]
        if this_class_name not in classes: continue
        
        source_file, line_hits, method_hits = parse_class_coverage(data, classes[this_class_name])
        if not source_file: continue
        
        abs_path = None
        pkg_path = os.path.dirname(this_class_name)
        for sd in source_dirs:
            candidate = os.path.join(sd, pkg_path, source_file)
            if os.path.exists(candidate):
                abs_path = os.path.abspath(candidate)
                break
        
        if not abs_path: continue
        
        if abs_path not in lcov: lcov[abs_path] = {}
        for l, h in line_hits.items():
            if l not in lcov[abs_path]: lcov[abs_path][l] = h
            else: lcov[abs_path][l] += h
            
        if abs_path not in lcov_methods: lcov_methods[abs_path] = {}
        for m, (fl, h) in method_hits.items():
            if m not in lcov_methods[abs_path]: lcov_methods[abs_path][m] = [fl, h]
            else: lcov_methods[abs_path][m][1] += h
            
    with open(output, 'w') as f:
        for path, hits in lcov.items():
            f.write(f"SF:{path}\n")
            m_hits = lcov_methods.get(path, {})
            fnf = len(m_hits)
            fnh = 0
            sorted_methods = sorted(m_hits.items(), key=lambda x: (x[1][0], x[0]))
            for m, (fl, h) in sorted_methods:
                f.write(f"FN:{fl},{m}\n")
            for m, (fl, h) in sorted_methods:
                f.write(f"FNDA:{h},{m}\n")
                if h > 0: fnh += 1
            if fnf > 0:
                f.write(f"FNF:{fnf}\n")
                f.write(f"FNH:{fnh}\n")
            lh = 0
            for l, h in sorted(hits.items()):
                f.write(f"DA:{l},{h}\n")
                if h > 0: lh += 1
            f.write(f"LF:{len(hits)}\n")
            f.write(f"LH:{lh}\n")
            f.write("end_of_record\n")

def process_xml(xml_file, output, chip_root):
    tree = ET.parse(xml_file)
    root = tree.getroot()
    with open(output, 'w') as f:
        for package in root.findall('package'):
            pkg_name = package.get('name')
            for sourcefile in package.findall('sourcefile'):
                src_name = sourcefile.get('name')
                filepath = os.path.join(chip_root, pkg_name, src_name)
                f.write('SF:' + filepath + '\n')
                lf = 0
                lh = 0
                for line in sourcefile.findall('line'):
                    nr = line.get('nr')
                    ci = line.get('ci')
                    f.write(f'DA:{nr},{ci}\n')
                    lf += 1
                    if int(ci) > 0: lh += 1
                f.write(f'LF:{lf}\n')
                f.write(f'LH:{lh}\n')
                f.write('end_of_record\n')

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--ec-dir', action='append', help='Directory with .ec files')
    parser.add_argument('--class-dirs', help='Comma separated class dirs')
    parser.add_argument('--source-dirs', help='Comma separated source dirs')
    parser.add_argument('--output', required=True)
    parser.add_argument('--chip-root', required=True)
    parser.add_argument('xml_file', nargs='?', help='Legacy XML file')
    args = parser.parse_args()
    
    if args.xml_file:
        process_xml(args.xml_file, args.output, args.chip_root)
    else:
        class_dirs = args.class_dirs.split(',') if args.class_dirs else []
        source_dirs = args.source_dirs.split(',') if args.source_dirs else []
        process_ec(args.ec_dir, class_dirs, source_dirs, args.output, args.chip_root)
