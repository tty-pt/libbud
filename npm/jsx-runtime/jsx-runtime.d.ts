import { BudNode } from "../dist/index.js";

export const Fragment: unique symbol;

export type Key = string | number | bigint;

export type Ref<T = any> = ((instance: T | null) => void) | { current: T | null };

export interface Attributes {
  key?: Key;
  ref?: Ref<any>;
  class?: string;
  className?: string;
  htmlFor?: string;
  id?: string;
  style?: string | Record<string, string | number>;
  title?: string;
  role?: string;
  tabIndex?: number;
  dangerouslySetInnerHTML?: { __html: string };
  onClick?: (event: any) => void;
  onDoubleClick?: (event: any) => void;
  onInput?: (event: any) => void;
  onChange?: (event: any) => void;
  onSubmit?: (event: any) => void;
  onKeyDown?: (event: any) => void;
  onKeyUp?: (event: any) => void;
  onFocus?: (event: any) => void;
  onBlur?: (event: any) => void;
  onMouseEnter?: (event: any) => void;
  onMouseLeave?: (event: any) => void;
  [key: string]: any;
}

export type Child = BudNode | string | number | boolean | null | undefined;
export type Children = Child | Child[];

export interface Props extends Attributes {
  children?: Children;
}

export type Component<P = {}> = (props: P & { key?: Key }) => BudNode | string | number | boolean | null | undefined;

export function jsx(
  type: string | Component<any> | typeof Fragment,
  props?: Props,
  key?: Key
): BudNode;

export function jsxs(
  type: string | Component<any> | typeof Fragment,
  props?: Props,
  key?: Key
): BudNode;

export function jsxDEV(
  type: string | Component<any> | typeof Fragment,
  props?: Props,
  key?: Key
): BudNode;

export namespace JSX {
  export type Element = BudNode;
  export interface ElementClass {
    render(): BudNode;
  }
  export interface ElementAttributesProperty {
    props: {};
  }
  export interface ElementChildrenAttribute {
    children: {};
  }
  export interface IntrinsicAttributes {
    key?: Key;
  }
  export interface IntrinsicElements {
    [elemName: string]: Props;
  }
}

export function useState<S>(
  initialState: S | (() => S)
): [S, (action: S | ((prevState: S) => S)) => void];

export function useReducer<S, A>(
  reducer: (state: S, action: A) => S,
  initialArg: S
): [S, (action: A) => void];
export function useReducer<S, I, A>(
  reducer: (state: S, action: A) => S,
  initialArg: I,
  init: (arg: I) => S
): [S, (action: A) => void];

export function useRef<T>(initialValue: T): { current: T };

export function useMemo<T>(factory: () => T, deps?: readonly any[]): T;

export function useCallback<T extends (...args: any[]) => any>(fn: T, deps?: readonly any[]): T;

export function useEffect(effect: () => void | (() => void), deps?: readonly any[]): void;

export function memo<P>(
  component: Component<P>,
  compare?: (prevProps: P, nextProps: P) => boolean
): Component<P>;

export function shallowEqual(a: any, b: any): boolean;

export function render(vnode: Child, container: Element): void;

export function flushSync(): void;

export function setRootScheduler(fn: () => void): void;
