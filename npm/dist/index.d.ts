export declare class BudNode {
  tag: string;
  attrs: Record<string, string>;
  children: Array<BudNode | string>;
  text: string;
  constructor(tag: string, attrs?: Record<string, string>, children?: Array<BudNode | string>);
  attr(key: string, val: string): this;
  child(node: BudNode | string): this;
  setText(text: string): this;
  render(): string;
}

export declare function h(tag: string, attrs?: Record<string, string>, children?: Array<BudNode | string>): BudNode;

export interface VdomPatch {
  type: "MOUNT" | "UNMOUNT" | "REPLACE" | "SET_TEXT";
  node?: BudNode;
  text?: string;
}

export declare function diffVdom(oldTree: BudNode | null, newTree: BudNode | null): VdomPatch[];

export declare function getBudWasm(): WebAssembly.Instance;

export declare function version(): string;
